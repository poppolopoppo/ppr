module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"
// Internal engine header, not installed: the exact binary16 codec the
// presentation row's motion pair shares with the resolved-cell codecs.
#include "../../app/renderer/App.Renderer.Binary16.h"
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

    // ------------------------------------------------------------------
    // particle orientation target/slew mirror
    // ------------------------------------------------------------------
    // The prepare shader owns this arithmetic. These mirrors restate the
    // design's stated target and slew so the encoded limits are checked without a
    // device readback; they are independent of the production code and must be
    // kept formula-for-formula with grid_particle_prepare.slang
    // (updateOrientationRow and its helpers).

    inline constexpr u32 kOrientationAngleCount = 65536u;
    inline constexpr int kOrientationAngleSlewCodes = 2048;
    inline constexpr int kOrientationAmplitudeSlewUnits = 32;
    inline constexpr double kOrientationDegreesPerCode = 180.0 / 65536.0;

    [[nodiscard]] Double2 doubledAxis(const Double2 unit) noexcept {
        return {unit.x * unit.x - unit.y * unit.y, 2.0 * unit.x * unit.y};
    }

    struct OrientationTarget {
        bool m_has_axis{};
        Double2 m_axis{};
        double m_bias{};
        double m_stretch{};
    };

    // q = f D(n) + (1-f) a D(t), W = f + (1-f) a, h = |q| / max(W, 1e-8).
    // A vanishing q retains no axis and targets zero bias and stretch.
    [[nodiscard]] OrientationTarget orientationTarget(
        const double flow_confidence, const Double2 motion_axis, const double tangent_confidence,
        const Double2 tangent_axis) noexcept {
        const Double2 motion_doubled = doubledAxis(motion_axis);
        const Double2 tangent_doubled = doubledAxis(tangent_axis);
        const double qx = flow_confidence * motion_doubled.x +
                          (1.0 - flow_confidence) * tangent_confidence * tangent_doubled.x;
        const double qy = flow_confidence * motion_doubled.y +
                          (1.0 - flow_confidence) * tangent_confidence * tangent_doubled.y;
        const double total = flow_confidence + (1.0 - flow_confidence) * tangent_confidence;
        const double magnitude = std::sqrt(qx * qx + qy * qy);
        if (!(magnitude > 1.0e-8))
            return {};

        const double shared = std::clamp(magnitude / std::max(total, 1.0e-8), 0.0, 1.0);
        return {true, {qx / magnitude, qy / magnitude}, tangent_confidence * shared, flow_confidence * shared};
    }

    // Unoriented half-angle of q, modulo pi, rounded to nearest angle16 code.
    [[nodiscard]] u32 orientationAngleCodes(const Double2 axis) noexcept {
        double angle = 0.5 * std::atan2(axis.y, axis.x);
        if (angle < 0.0)
            angle += kPi;
        return static_cast<u32>(std::floor(angle / kPi * static_cast<double>(kOrientationAngleCount) + 0.5)) &
               (kOrientationAngleCount - 1u);
    }

    [[nodiscard]] u32 orientationAmplitudeUnits(const double value) noexcept {
        return static_cast<u32>(std::floor(std::clamp(value, 0.0, 1.0) * 255.0 + 0.5));
    }

    struct OrientationState {
        u32 m_angle{};
        u32 m_bias{};
        u32 m_stretch{};
        bool m_initialized{};
    };

    // New identity: target angle (zero if none) and B = S = 0, then ramp.
    // Otherwise the canonical angle takes the shortest signed modulo-65536 delta
    // (positive at the exact 32768 tie), clamped to +-2048 codes, and each
    // amplitude takes at most +-32 units.
    [[nodiscard]] OrientationState slewOrientation(
        OrientationState previous, const bool has_axis, const u32 target_angle, const u32 bias_stop,
        const u32 stretch_stop) noexcept {
        if (!previous.m_initialized) {
            previous.m_angle = has_axis ? target_angle : 0u;
            previous.m_bias = 0u;
            previous.m_stretch = 0u;
            previous.m_initialized = true;
        }

        const i32 angle_stop = static_cast<i32>(has_axis ? target_angle : previous.m_angle);
        i32 delta = angle_stop - static_cast<i32>(previous.m_angle);
        constexpr i32 count = static_cast<i32>(kOrientationAngleCount);
        delta = ((delta % count) + count) % count;
        if (delta > 32768)
            delta -= count;

        delta = std::clamp(delta, -kOrientationAngleSlewCodes, kOrientationAngleSlewCodes);
        previous.m_angle = static_cast<u32>((static_cast<i32>(previous.m_angle) + delta) & 0xffff);
        const auto step = [](const u32 from, const u32 to) noexcept {
            const i32 value = std::clamp(
                static_cast<i32>(to) - static_cast<i32>(from), -kOrientationAmplitudeSlewUnits,
                kOrientationAmplitudeSlewUnits);
            return static_cast<u32>(std::clamp(static_cast<i32>(from) + value, 0, 255));
        };
        previous.m_bias = step(previous.m_bias, has_axis ? bias_stop : 0u);
        previous.m_stretch = step(previous.m_stretch, has_axis ? stretch_stop : 0u);
        return previous;
    }

    // Decoded unit axis for one canonical angle16, exactly as the draw decodes it:
    // the vertex stage evaluates the trig pair in binary32. No half pair is stored.
    [[nodiscard]] Double2 decodedAxis(const u32 angle_codes) noexcept {
        const float radians = static_cast<float>(
            static_cast<double>(angle_codes) * kPi / static_cast<double>(kOrientationAngleCount));
        return {static_cast<double>(std::cos(radians)), static_cast<double>(std::sin(radians))};
    }

    // Angle between two unoriented axes, in degrees, in [0, 90].
    [[nodiscard]] double axisDegrees(const Double2 a, const Double2 b) noexcept {
        double difference = std::atan2(a.y, a.x) - std::atan2(b.y, b.x);
        difference = std::fmod(difference, kPi);
        if (difference > kPi / 2.0)
            difference -= kPi;
        if (difference < -kPi / 2.0)
            difference += kPi;
        return std::abs(difference) * 180.0 / kPi;
    }

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

        // Same position and extent through the perspective projection: the
        // background must keep the authored unlit fallback, never the lit path.
        CameraModel perspective_model = model;
        perspective_model.m_camera_mode = ECameraProjection::perspective;
        Camera perspective_camera{ECameraProjection::perspective};
        perspective_camera.updateModel(TimeSpan{}, perspective_model, Viewport{PixelRect{0, 0, kEdge, kEdge}});
        const auto perspective_snapshot = perspective_camera.getSnapshot();

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

        const auto capture = [&](const CameraSnapshot &camera_snapshot, const lighting::CH &ch) {
            PPR_TEST_ASSERT(not grid.setLightingBuffersForTest(resolved_buffer, make_ch(ch)));
            PPR_TEST_ASSERT(not grid.update(TimeSpan{}, camera_snapshot, false));
            const auto prepare = [&](const PreRenderContext &context) -> std::error_code {
                if (const auto ec = grid.prepareBackgroundInputs(context, camera_snapshot, viewport, scissor)) {
                    return ec;
                }

                return grid.prepareBackground(context, camera_snapshot, viewport, scissor);
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
        const auto lit = capture(snapshot, uniformCH(float3{0.5f, 0.25f, 0.125f}));
        // This fixture never runs the categorical draw, so the fallback buffer
        // can only have come from the background preparation path.
        PPR_TEST_ASSERT(grid.lightingFallbackForTest() != nullptr);
        const auto lit_center = pixel(lit, kEdge / 2u, kEdge / 2u);
        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL background_receiver lit=({:.4f},{:.4f},{:.4f},{:.4f})", lit_center[0],
                lit_center[1], lit_center[2], lit_center[3]));
        PPR_TEST_ASSERT(std::abs(lit_center[0] - 0.5f) <= 0.003f);
        PPR_TEST_ASSERT(std::abs(lit_center[1] - 0.25f) <= 0.003f);
        PPR_TEST_ASSERT(std::abs(lit_center[2] - 0.125f) <= 0.003f);
        PPR_TEST_ASSERT(std::abs(lit_center[3] - 1.0f) <= 0.003f);

        // A perspective camera has no affine z=0 receiving plane, so the same
        // staged background must use the authored unlit fallback: the nonzero
        // uniform field leaves the white layer white instead of black.
        const auto unlit = capture(perspective_snapshot, uniformCH(float3{0.5f, 0.25f, 0.125f}));
        const auto unlit_center = pixel(unlit, kEdge / 2u, kEdge / 2u);
        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL background_receiver unlit=({:.4f},{:.4f},{:.4f},{:.4f})", unlit_center[0],
                unlit_center[1], unlit_center[2], unlit_center[3]));
        PPR_TEST_ASSERT(std::abs(unlit_center[0] - 1.0f) <= 0.003f);
        PPR_TEST_ASSERT(std::abs(unlit_center[1] - 1.0f) <= 0.003f);
        PPR_TEST_ASSERT(std::abs(unlit_center[2] - 1.0f) <= 0.003f);
        PPR_TEST_ASSERT(std::abs(unlit_center[3] - 1.0f) <= 0.003f);

        // A zero field yields zero reflected HDR while the layer stays opaque.
        const auto dark = capture(snapshot, lighting::CH{});
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

        // jump: the six empty classes are fills on their literal palette entries
        // and stored class 0 is the occupancy colour (white), never black. These
        // sampled cells are surrounded by class 0, so no square bound is drawn
        // beside them and the fill fills the texel; at this 16-cell-per-pixel
        // zoom the bound fade is also off.
        const std::array<float3, 6u> jump_colors{
            float3{0.80f, 0.08f, 0.08f}, float3{0.95f, 0.45f, 0.05f}, float3{0.92f, 0.92f, 0.08f},
            float3{0.10f, 0.80f, 0.20f}, float3{0.10f, 0.35f, 0.95f}, float3{0.85f, 0.15f, 0.85f}};
        for (u32 index = 0u; index < jump_colors.size(); ++index) {
            InspectionRenderer::assertCategorical(
                _, InspectionRenderer::texel(jump_pixels, index, 63u), jump_colors[index]);
        }

        // Pixel 6 reads cell (104,8), stored class 0: occupancy, distinct from
        // every empty class at this zoom.
        const auto jump_occupancy = InspectionRenderer::texel(jump_pixels, 6u, 63u);
        InspectionRenderer::assertCategorical(_, jump_occupancy, float3{1.0f, 1.0f, 1.0f});
        for (const float3 &color: jump_colors) {
            const std::array<int, 3u> encoded{
                static_cast<int>(std::lround(inspectionSrgbEncode(color.x) * 255.0f)),
                static_cast<int>(std::lround(inspectionSrgbEncode(color.y) * 255.0f)),
                static_cast<int>(std::lround(inspectionSrgbEncode(color.z) * 255.0f))};
            const int separation = (std::max) (std::abs(static_cast<int>(jump_occupancy[0]) - encoded[0]),
                (std::max) (std::abs(static_cast<int>(jump_occupancy[1]) - encoded[1]),
                    std::abs(static_cast<int>(jump_occupancy[2]) - encoded[2])));
            PPR_TEST_ASSERT(separation >= 24);
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

    // The jump view shows the selected origin-aligned square, not a distance
    // band. One occupied cell at (31,31) makes row 31 read class 16/8/4/2/1 up
    // to the solid and class 32 immediately to its right, because every empty
    // square is aligned to the origin rather than grown from the obstacle.
    PPR_UNIT_TEST (inspection_jump_view_labels_occupancy_and_selected_square_bounds) {
        Fixture fixture;
        fixture.initialize();
        PPR_DEFER {
            fixture.shutdown();
        };
        auto &device = fixture.device();
        InspectionRenderer renderer{fixture};

        constexpr u32 edge = WorldLightingPass::kWorldEdge;
        const auto cell = [](const u32 x, const u32 y) { return static_cast<std::size_t>(y) * edge + x; };

        InspectionResources inputs{};
        std::vector<u32> resolved(static_cast<std::size_t>(edge) * edge, 0u);
        std::vector<u8> jump(static_cast<std::size_t>(edge) * edge, 32u);
        jump[cell(31u, 31u)] = 0u;
        resolved[cell(31u, 31u)] = *lighting::packCell(1u, 300.0);

        std::array<GridTerrainVisual, 2u> visuals{};
        visuals[1u].m_material_id = 1u;
        visuals[1u].m_color_rgba = 0xffff'ffffu;

        // The jump view decodes only the published class texture. The remaining
        // bindings exist because the shared overlay program declares them for
        // its sibling views.
        const auto ch = lighting::packCH(uniformCH(float3{0.0f}));
        PPR_TEST_ASSERT(ch.has_value());
        const auto interval = lighting::packInterval(lighting::Interval{});
        PPR_TEST_ASSERT(interval.has_value());
        std::array<lighting::PackedCH, 64u> fine{};
        for (auto &record: fine) {
            record = *ch;
        }

        inputs.m_resolved = makeBuffer(device, resolved.size() * sizeof(u32), sizeof(u32), resolved.data());
        inputs.m_visuals = makeBuffer(device, sizeof(visuals), sizeof(GridTerrainVisual), visuals.data());
        inputs.m_fine_ch = makeBuffer(device, sizeof(fine), sizeof(lighting::PackedCH), fine.data());
        inputs.m_source = makeBuffer(device, sizeof(u32), sizeof(u32), resolved.data());
        inputs.m_cascades = makeBuffer(
            device, sizeof(lighting::PackedInterval), sizeof(lighting::PackedInterval), &*interval);

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

        // Eight window pixels per native cell: a class fill sits several pixels
        // from its square bounds and each bound is a one-pixel line. The
        // translated camera leaves an outside-domain corner on screen.
        const float cells_per_metre = 1.0f;
        const int2 extent{512, 512};
        const CameraSnapshot camera = inspectionMetricCamera(
            cells_per_metre, float2{-8.0f, -8.0f}, float2{64.0f, 64.0f});
        const auto pixels = renderer.draw(
            inputs, camera, static_cast<u32>(ELightingDiagnostic::jump), 1u, extent, cells_per_metre);

        const auto texelAt = [&](const float2 native) {
            const float2 pixel = inspectionPixelAt(camera, native, cells_per_metre, extent);
            return InspectionRenderer::texel(
                pixels, static_cast<u32>(std::floor(pixel.x)), static_cast<u32>(std::floor(pixel.y)));
        };
        const auto channelSum = [](const std::array<u8, 4u> &texel) {
            return static_cast<int>(texel[0]) + static_cast<int>(texel[1]) + static_cast<int>(texel[2]);
        };

        // Fill: every empty class keeps its exact palette entry at a cell
        // centre, so the six classes stay comparable with earlier captures.
        const std::array<float3, 6u> classes{
            float3{0.80f, 0.08f, 0.08f}, float3{0.95f, 0.45f, 0.05f}, float3{0.92f, 0.92f, 0.08f},
            float3{0.10f, 0.80f, 0.20f}, float3{0.10f, 0.35f, 0.95f}, float3{0.85f, 0.15f, 0.85f}};
        const std::array<u32, 6u> class_cells{30u, 28u, 24u, 16u, 4u, 40u};
        for (u32 index = 0u; index < classes.size(); ++index) {
            const float2 centre{static_cast<float>(class_cells[index]) + 0.5f, 31.5f};
            InspectionRenderer::assertCategorical(_, texelAt(centre), classes[index]);
        }

        // Occupancy: class 0 is the occupied publication, so the solid reads as
        // the flat occupancy colour and stays distinct from every empty class.
        const auto occupied = texelAt(float2{31.5f, 31.5f});
        InspectionRenderer::assertCategorical(_, occupied, float3{1.0f, 1.0f, 1.0f});
        for (const float3 &color: classes) {
            const std::array<int, 3u> encoded{
                static_cast<int>(std::lround(inspectionSrgbEncode(color.x) * 255.0f)),
                static_cast<int>(std::lround(inspectionSrgbEncode(color.y) * 255.0f)),
                static_cast<int>(std::lround(inspectionSrgbEncode(color.z) * 255.0f))};
            const int separation = (std::max) (std::abs(static_cast<int>(occupied[0]) - encoded[0]),
                (std::max) (std::abs(static_cast<int>(occupied[1]) - encoded[1]),
                    std::abs(static_cast<int>(occupied[2]) - encoded[2])));
            PPR_TEST_ASSERT(separation >= 24);
        }

        // Outside-domain black remains distinct from occupied white.
        const auto outside = texelAt(float2{-4.0f, -4.0f});
        PPR_TEST_ASSERT(outside[0] == 0u and outside[1] == 0u and outside[2] == 0u);

        // Bounds are drawn only where the selected square changes. Cell (15,31)
        // selects the 16-square [0,16)x[16,32), cell (16,31) the 8-square
        // [16,24)x[24,32); each sees an empty cell across their shared bound, so
        // one line pixel there is darker than either fill while both fills keep
        // their exact class colour, including the one-cell square of cell (30,31)
        // sampled above.
        const auto left_fill = texelAt(float2{15.5f, 31.5f});
        const auto right_fill = texelAt(float2{16.5f, 31.5f});
        InspectionRenderer::assertCategorical(_, left_fill, classes[4u]);
        InspectionRenderer::assertCategorical(_, right_fill, classes[3u]);
        const int softened = (std::min) (channelSum(left_fill), channelSum(right_fill)) - 12;
        PPR_TEST_ASSERT(channelSum(texelAt(float2{16.0f, 31.5f})) < softened);

        // The bound against the solid at x=32 is not a square change: the cell
        // across it is occupancy, so that fill reaches the solid undlined.
        InspectionRenderer::assertCategorical(_, texelAt(float2{32.0f, 31.5f}), classes[5u]);

        // Inside one selected square a cell edge is not a square bound: cells
        // (40,31) and (41,31) share the 32-square [32,64)x[0,32), so a sample
        // straddling their edge keeps the exact class fill instead of a grid.
        InspectionRenderer::assertCategorical(_, texelAt(float2{41.0f, 31.5f}), classes[5u]);
    };

    // ------------------------------------------------------------------
    // packed particle presentation row
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (presentation_tint_rgb9e5_matches_the_authored_palette_bytes) {
        // Authored palette tints: zero, full, dominant, dim and minimum-visible
        // channels. The particle row no longer stores a tint copy (word 16..19 is
        // the stable UV seed and shading decodes the palette bytes directly), so
        // this pins the shared codec's own byte-error bounds and rejections.
        constexpr std::array<u32, 8u> authored{
            0x000000ffu, 0xffffffffu, 0xff0080ffu, 0x010203ffu, 0x11c840ffu, 0x808080ffu, 0xfe0102ffu, 0x7f7f7fffu};
        for (const u32 rgba: authored) {
            const float3 source{
                static_cast<float>((rgba >> 24u) & 255u) / 255.0f, static_cast<float>((rgba >> 16u) & 255u) / 255.0f,
                static_cast<float>((rgba >> 8u) & 255u) / 255.0f};
            const Expected<u32> word = lighting::packRGB9E5(source);
            PPR_TEST_ASSERT(word.has_value());
            const float3 decoded = lighting::unpackRGB9E5(*word);
            const std::array<float, 3u> actual{decoded.x, decoded.y, decoded.z};
            const std::array<float, 3u> expected{source.x, source.y, source.z};
            std::array<float, 3u> error{};
            for (u32 channel = 0u; channel < expected.size(); ++channel) {
                error[channel] = std::abs(actual[channel] - expected[channel]);
                // At least as fine as the RGBA8 source it copies.
                PPR_TEST_ASSERT(error[channel] <= 1.0f / 255.0f);
            }

            // The dominant channel keeps its claimed 1/256 absolute bound.
            PPR_TEST_ASSERT(*std::ranges::max_element(error) <= 1.0f / 256.0f);
            _.log(
                std::format(
                    "WORLD_LIGHTING_MATERIAL presentation_tint rgba=0x{:08x} word=0x{:08x} error=({:.3e},{:.3e},"
                    "{:.3e})",
                    rgba, *word, error[0u], error[1u], error[2u]));
        }

        // An invalid tint is refused rather than encoded into the row.
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float infinity = std::numeric_limits<float>::infinity();
        const auto negative = lighting::packRGB9E5(float3{0.0f, -1.0f, 0.0f});
        const auto not_a_number = lighting::packRGB9E5(float3{nan, 0.0f, 0.0f});
        const auto infinite = lighting::packRGB9E5(float3{infinity, 0.0f, 0.0f});
        const auto overflow = lighting::packRGB9E5(float3{0.0f, 0.0f, 65'409.0f});
        PPR_TEST_ASSERT(not negative.has_value() and negative.error() == std::errc::invalid_argument);
        PPR_TEST_ASSERT(not not_a_number.has_value() and not_a_number.error() == std::errc::invalid_argument);
        PPR_TEST_ASSERT(not infinite.has_value() and infinite.error() == std::errc::invalid_argument);
        PPR_TEST_ASSERT(not overflow.has_value() and overflow.error() == std::errc::result_out_of_range);
    };

    PPR_UNIT_TEST (presentation_motion_half_round_trips_and_clamps) {
        // Finite displacements, both signs, including a half subnormal, encode
        // and decode within one binary16 ulp.
        constexpr std::array<double, 8u> finite{-63.9, -1.0, -0.1, -0x1p-20, 0.0, 0.1, 0.25, 63.9};
        for (const double value: finite) {
            const u16 bits = ::pP::details::encodeHalf(value);
            const float decoded = ::pP::details::decodeHalf(bits);
            const double magnitude = std::abs(value);
            const double ulp = magnitude < 0x1p-14 ? 0x1p-24 : std::ldexp(1.0, std::ilogb(magnitude) - 10);
            PPR_TEST_ASSERT(std::abs(static_cast<double>(decoded) - value) <= ulp);
            _.log(
                std::format(
                    "WORLD_LIGHTING_MATERIAL presentation_motion value={:.6f} bits=0x{:04x} decoded={:.6f}", value,
                    bits, decoded));
        }

        // The +-64 cells/step clamp endpoints are exactly representable, so the
        // shader clamp stores the endpoint and not its neighbour.
        PPR_TEST_ASSERT(::pP::details::encodeHalf(64.0) == 0x5400u);
        PPR_TEST_ASSERT(::pP::details::encodeHalf(-64.0) == 0xd400u);
        PPR_TEST_ASSERT(::pP::details::decodeHalf(0x5400u) == 64.0f);
        PPR_TEST_ASSERT(::pP::details::decodeHalf(0xd400u) == -64.0f);
        // Negative zero canonicalizes, so a stationary row cannot store -0.
        PPR_TEST_ASSERT(::pP::details::encodeHalf(-0.0) == 0u);
    };

    PPR_UNIT_TEST (presentation_material_word_reconstructs_the_canonical_pack_cell_word) {
        constexpr std::array<double, 5u> temperatures{0.0, 293.15, 900.0, 3273.0, 65'504.0};
        for (u32 material = 1u; material <= 8u; ++material) {
            for (const double kelvin: temperatures) {
                const auto canonical_word = lighting::packCell(static_cast<u16>(material), kelvin);
                PPR_TEST_ASSERT(canonical_word.has_value());
                const u32 canonical = *canonical_word;
                for (const u32 class_flags: {0u, 1u, 2u, 3u}) {
                    // material 12 | class 4 << 12 | binary16 temperature 16 << 16
                    const u32 row = (canonical & 0xffffu) | (class_flags << 12u) | (canonical & 0xffff'0000u);
                    PPR_TEST_ASSERT((row & 0xfffu) == material);
                    PPR_TEST_ASSERT(((row >> 12u) & 0xfu) == class_flags);
                    // canonical = material | (temperature << 16)
                    PPR_TEST_ASSERT(((row & 0xfffu) | (row & 0xffff'0000u)) == canonical);
                }
            }
        }

        // The 12-bit domain is explicit. A truncating packer would alias the
        // over-range id onto material 1, which is why it is rejected instead.
        PPR_TEST_ASSERT(kGridParticleMaterialLimit == 4096u);
        PPR_TEST_ASSERT((kGridParticleMaterialLimit & 0xfffu) == 0u);
        PPR_TEST_ASSERT(((kGridParticleMaterialLimit + 1u) & 0xfffu) == 1u);
    };

    PPR_UNIT_TEST (presentation_position_precision_budget_pins_the_far_corner_quantum) {
        // span64 renders 11.25 px per published 1/3 m cell.
        constexpr double cell_metres = 1.0 / 3.0;
        constexpr double pixels_per_cell = 11.25;
        constexpr double budget_px = 1.0e-2;

        for (const float corner: {341.33f, 1365.3f}) {
            const double quantum_m = static_cast<double>(std::nextafter(corner, std::numeric_limits<float>::max())) -
                                     static_cast<double>(corner);
            const double quantum_px = quantum_m / cell_metres * pixels_per_cell;
            _.log(
                std::format(
                    "WORLD_LIGHTING_MATERIAL presentation_position corner={:.2f}m quantum={:.3e}m px={:.3e} "
                    "budget={:.3e}",
                    corner, quantum_m, quantum_px, budget_px));
            PPR_TEST_ASSERT(quantum_px <= budget_px);
        }

        // The rejected alternatives must exceed the same budget, so replacing the
        // encoding fails here instead of shrinking precision silently: half's
        // quantum at 341.33 m (2^8 <= x < 2^9) is 0.25 m, and 16-bit fixed point
        // over 1024 cells is 1/64 cell.
        PPR_TEST_ASSERT(0.25 / cell_metres * pixels_per_cell > budget_px);
        PPR_TEST_ASSERT((1.0 / 64.0) * pixels_per_cell > budget_px);
    };

    PPR_UNIT_TEST (presentation_row_layout_matches_the_cpu_twin) {
        Fixture fixture{};
        fixture.initialize();
        PPR_DEFER {
            fixture.shutdown();
        };

        // CPU twin: 24 bytes, 8-aligned, packed in the documented order.
        PPR_TEST_ASSERT(sizeof(ParticleGpu) == 24u);
        PPR_TEST_ASSERT(alignof(ParticleGpu) == 8u);
        PPR_TEST_ASSERT(PPR_OFFSETOF(ParticleGpu, m_position) == 0u);
        PPR_TEST_ASSERT(PPR_OFFSETOF(ParticleGpu, m_radius_m) == 8u);
        PPR_TEST_ASSERT(PPR_OFFSETOF(ParticleGpu, m_material_flags_temperature) == 12u);
        PPR_TEST_ASSERT(PPR_OFFSETOF(ParticleGpu, m_uv_seed) == 16u);
        PPR_TEST_ASSERT(PPR_OFFSETOF(ParticleGpu, m_motion) == 20u);

        // Reflected on the device target this fixture initialized, so the same
        // case proves the layout on Vulkan and on D3D12.
        const auto verify = [&](const char *const file, const char *const module_name, const char *const parameter_name,
                                const std::span<const std::pair<const char *, u32>> fields) {
            shader::SharedModule module{};
            PPR_TEST_ASSERT(not fixture.m_shaders->loadModuleFromFile(
                std::filesystem::current_path() / "shaders" / file, module_name, module.writeRef()));
            auto *const layout = module->getLayout();
            PPR_TEST_ASSERT(layout != nullptr);
            slang::TypeLayoutReflection *element = nullptr;
            for (unsigned index = 0u; index < layout->getParameterCount(); ++index) {
                auto *const parameter = layout->getParameterByIndex(index);
                if (std::string_view{parameter->getName()} == parameter_name) {
                    element = parameter->getTypeLayout()->getElementTypeLayout();
                    break;
                }
            }

            PPR_TEST_ASSERT(element != nullptr);
            PPR_TEST_ASSERT(element->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM) == sizeof(ParticleGpu));
            PPR_TEST_ASSERT(element->getFieldCount() == static_cast<unsigned>(fields.size()));
            for (const auto &[name, offset]: fields) {
                bool matched = false;
                for (unsigned index = 0u; index < element->getFieldCount(); ++index) {
                    auto *const field = element->getFieldByIndex(index);
                    if (std::string_view{field->getName()} != name) {
                        continue;
                    }

                    const u32 measured = static_cast<u32>(field->getOffset(SLANG_PARAMETER_CATEGORY_UNIFORM));
                    _.log(
                        std::format(
                            "WORLD_LIGHTING_MATERIAL presentation_row {} field={} offset={} expected={} size={}",
                            module_name, name, measured, offset,
                            field->getTypeLayout()->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM)));
                    PPR_TEST_ASSERT(measured == offset);
                    matched = true;
                    break;
                }

                PPR_TEST_ASSERT(matched);
            }
        };

        constexpr std::array<std::pair<const char *, u32>, 5u> writer_fields{{
            {"position", 0u},
            {"radius", 8u},
            {"material_flags_temperature", 12u},
            {"uv_seed", 16u},
            {"motion", 20u},
        }};
        constexpr std::array<std::pair<const char *, u32>, 5u> reader_fields{{
            {"m_position", 0u},
            {"m_radius_m", 8u},
            {"m_material_flags_temperature", 12u},
            {"m_uv_seed", 16u},
            {"m_motion", 20u},
        }};
        verify("grid_particle_prepare.slang", "grid_particle_prepare", "g_packed", writer_fields);
        verify("grid_tiles.slang", "grid_tiles", "g_particles", reader_fields);
    };

    // ------------------------------------------------------------------
    // per-particle orientation target and slew
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (particle_orientation_target_agrees_and_cancels) {
        const Double2 axis{std::sqrt(0.5), std::sqrt(0.5)};
        // Agreeing cues: f = a = 0.5 over one axis gives h = 1 and both stored
        // amplitudes at their independent maxima (the withdrawn combined cap is
        // not reintroduced).
        const auto agreeing = orientationTarget(0.5, axis, 0.5, axis);
        PPR_TEST_ASSERT(agreeing.m_has_axis);
        PPR_TEST_ASSERT(std::abs(agreeing.m_bias - 0.5) <= 1.0e-12);
        PPR_TEST_ASSERT(std::abs(agreeing.m_stretch - 0.5) <= 1.0e-12);
        const u32 agreeing_codes = orientationAngleCodes(agreeing.m_axis);
        PPR_TEST_ASSERT(axisDegrees(decodedAxis(agreeing_codes), axis) <= 0.001);

        // Flow takes directional priority as f -> 1: the shared axis is the motion
        // axis, while the bias amplitude stays a * h because the surface itself is
        // still genuine (the two amplitudes are independent, not a split budget).
        const Double2 motion{1.0, 0.0};
        const Double2 tangent{0.0, 1.0};
        const auto flowing = orientationTarget(1.0, motion, 1.0, tangent);
        PPR_TEST_ASSERT(flowing.m_has_axis);
        PPR_TEST_ASSERT(std::abs(flowing.m_bias - 1.0) <= 1.0e-12);
        PPR_TEST_ASSERT(std::abs(flowing.m_stretch - 1.0) <= 1.0e-12);
        PPR_TEST_ASSERT(axisDegrees(decodedAxis(orientationAngleCodes(flowing.m_axis)), motion) <= 0.001);

        // Orthogonal unoriented axes are opposite doubled axes, so equal weights
        // cancel: q -> 0, no new axis, and both strengths target zero.
        const auto cancelled = orientationTarget(0.5, motion, 0.5, tangent);
        PPR_TEST_ASSERT(not cancelled.m_has_axis);
        PPR_TEST_ASSERT(cancelled.m_bias == 0.0 and cancelled.m_stretch == 0.0);
    };

    PPR_UNIT_TEST (particle_orientation_ramp_and_flip_take_eight_and_sixteen_updates) {
        const u32 full = orientationAmplitudeUnits(1.0);
        PPR_TEST_ASSERT(full == 255u);

        OrientationState ramped{};
        for (u32 update = 0u; update < 7u; ++update)
            ramped = slewOrientation(ramped, true, 0u, full, full);
        PPR_TEST_ASSERT(ramped.m_bias == 224u and ramped.m_stretch == 224u);
        ramped = slewOrientation(ramped, true, 0u, full, full);
        PPR_TEST_ASSERT(ramped.m_bias == 255u and ramped.m_stretch == 255u);

        OrientationState flipped{};
        flipped.m_initialized = true;
        for (u32 update = 0u; update < 15u; ++update)
            flipped = slewOrientation(flipped, true, 32768u, 0u, 0u);
        PPR_TEST_ASSERT(flipped.m_angle == 30720u);
        flipped = slewOrientation(flipped, true, 32768u, 0u, 0u);
        PPR_TEST_ASSERT(flipped.m_angle == 32768u);
    };

    PPR_UNIT_TEST (particle_orientation_slew_clamps_and_tie_direction) {
        // One publication turns at most 2048 canonical codes = 5.625 degrees.
        OrientationState rising{};
        rising.m_initialized = true;
        rising = slewOrientation(rising, true, 32768u, 0u, 0u);
        PPR_TEST_ASSERT(rising.m_angle == 2048u);
        PPR_TEST_ASSERT(std::abs(static_cast<double>(rising.m_angle) / 65536.0 * 180.0 - 5.625) <= 1.0e-12);

        // The exact 32768 tie is positive in both directions: from 0 toward 32768
        // and from 32768 toward 0 (whose raw delta is -32768).
        OrientationState tied{};
        tied.m_angle = 32768u;
        tied.m_initialized = true;
        const auto tied_up = slewOrientation(tied, true, 0u, 0u, 0u);
        PPR_TEST_ASSERT(tied_up.m_angle == 34816u);

        // One code short of the tie stays negative.
        OrientationState short_tie{};
        short_tie.m_angle = 32767u;
        short_tie.m_initialized = true;
        const auto short_up = slewOrientation(short_tie, true, 0u, 0u, 0u);
        PPR_TEST_ASSERT(short_up.m_angle == 30719u);

        // Each stored amplitude moves by at most 32 units.
        OrientationState bias{};
        bias = slewOrientation(bias, true, 0u, 255u, 0u);
        PPR_TEST_ASSERT(bias.m_bias == 32u);
        bias = slewOrientation(bias, false, 0u, 0u, 0u);
        PPR_TEST_ASSERT(bias.m_bias == 0u);

        OrientationState down{};
        down.m_bias = 255u;
        down.m_stretch = 255u;
        down.m_initialized = true;
        down = slewOrientation(down, false, 0u, 0u, 0u);
        PPR_TEST_ASSERT(down.m_bias == 223u and down.m_stretch == 223u);
    };

    PPR_UNIT_TEST (particle_orientation_zero_confidence_holds_the_axis_and_rounds) {
        // With no target axis the canonical angle is held and both amplitudes slew
        // toward zero, reaching the round response within eight updates.
        OrientationState state{};
        state.m_angle = 12345u;
        state.m_bias = 255u;
        state.m_stretch = 255u;
        state.m_initialized = true;
        for (u32 update = 0u; update < 7u; ++update)
            state = slewOrientation(state, false, 0u, 0u, 0u);
        PPR_TEST_ASSERT(state.m_angle == 12345u);
        PPR_TEST_ASSERT(state.m_bias == 31u and state.m_stretch == 31u);
        state = slewOrientation(state, false, 0u, 0u, 0u);
        PPR_TEST_ASSERT(state.m_angle == 12345u);
        PPR_TEST_ASSERT(state.m_bias == 0u and state.m_stretch == 0u);

        // A new identity with no target starts at angle zero and zero amplitudes.
        const auto born = slewOrientation(OrientationState{}, false, 0u, 0u, 0u);
        PPR_TEST_ASSERT(born.m_angle == 0u and born.m_bias == 0u and born.m_stretch == 0u);
    };

    PPR_UNIT_TEST (particle_orientation_decoded_axis_stays_inside_the_stated_bound) {
        // The record stores only the canonical angle, so the vertex stage evaluates
        // (cos, sin) in binary32. The absolute decoded-direction error is bounded by
        // binary32 trig tolerance (about 1e-5 degrees), and consecutive decoded axes
        // are therefore the 5.625-degree canonical step plus that tolerance; the
        // reported conservative screen-independent bound is 5.626 degrees.
        double worst_absolute = 0.0;
        for (const u32 codes: {0u, 1u, 2u, 4096u, 32768u, 49152u, 65535u}) {
            const double radians = static_cast<double>(codes) * kPi / static_cast<double>(kOrientationAngleCount);
            const Double2 canonical{std::cos(radians), std::sin(radians)};
            worst_absolute = std::max(worst_absolute, axisDegrees(decodedAxis(codes), canonical));
            PPR_TEST_ASSERT(axisDegrees(decodedAxis(codes), canonical) <= 0.001);
        }

        double worst_step = 0.0;
        for (const u32 codes: {0u, 1u, 2046u, 8192u, 32768u, 63488u}) {
            const u32 next = (codes + 2048u) & (kOrientationAngleCount - 1u);
            const double step = axisDegrees(decodedAxis(codes), decodedAxis(next));
            worst_step = std::max(worst_step, step);
            PPR_TEST_ASSERT(step <= 5.626);
        }

        PPR_TEST_ASSERT(std::abs(kOrientationDegreesPerCode * 2048.0 - 5.625) <= 1.0e-12);

        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL orientation_axis worst_absolute_degrees={:.6f} "
                "worst_step_degrees={:.6f}",
                worst_absolute, worst_step));
    };

    // ------------------------------------------------------------------
    // per-instance particle descriptions and shapes
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (particle_descriptions_use_the_rows_own_phase_material) {
        // Nonvisual selection-helper assertions: no device, no pixels. Two
        // solids in one cell and two liquids in one cell resolve to their own
        // palette entries, and the helper takes no cell argument, so no resolved
        // destination cell or winner material can substitute for the row's id.
        GridTerrainVisual solid_a{};
        solid_a.m_material_id = 11u;
        solid_a.m_texture_index = 3u;
        solid_a.m_normal_texture_index = 7u;
        solid_a.m_color_rgba = 0xff8040ffu;
        solid_a.m_roughness = 0.2f;
        solid_a.m_metallic = 0.1f;
        solid_a.m_surface_flags = kGridSurfaceParticleSolid;

        GridTerrainVisual solid_b = solid_a;
        solid_b.m_material_id = 12u;
        solid_b.m_texture_index = 4u;
        solid_b.m_normal_texture_index = 8u;
        solid_b.m_color_rgba = 0x40ff80ffu;
        solid_b.m_roughness = 0.7f;
        solid_b.m_metallic = 0.9f;

        GridTerrainVisual liquid_a{};
        liquid_a.m_material_id = 21u;
        liquid_a.m_texture_index = 5u;
        liquid_a.m_color_rgba = 0x2040ffffu;
        liquid_a.m_ior = 1.333f;
        liquid_a.m_refraction_depth_m = 0.2f;
        liquid_a.m_sigma_per_m[0] = 0.1f;
        liquid_a.m_sigma_per_m[1] = 0.2f;
        liquid_a.m_sigma_per_m[2] = 0.3f;
        liquid_a.m_surface_flags = kGridSurfaceParticleLiquid | kGridSurfaceOpticalBlend;

        GridTerrainVisual liquid_b = liquid_a;
        liquid_b.m_material_id = 22u;
        liquid_b.m_texture_index = 6u;
        liquid_b.m_color_rgba = 0xffff40ffu;
        liquid_b.m_ior = 1.51f;
        liquid_b.m_refraction_depth_m = 0.35f;
        liquid_b.m_sigma_per_m[0] = 0.9f;
        liquid_b.m_sigma_per_m[1] = 0.8f;
        liquid_b.m_sigma_per_m[2] = 0.7f;

        // The blend class and the phase are independent: an opaque liquid (the
        // magma-like case) carries the liquid phase without the optical-blend
        // bit, and an optical solid carries the solid phase with it.
        GridTerrainVisual opaque_liquid = liquid_a;
        opaque_liquid.m_material_id = 23u;
        opaque_liquid.m_color_rgba = 0xff2000ffu;
        opaque_liquid.m_ior = 1.2f;
        opaque_liquid.m_surface_flags = kGridSurfaceParticleLiquid;

        GridTerrainVisual optical_solid = solid_a;
        optical_solid.m_material_id = 13u;
        optical_solid.m_color_rgba = 0x80ffffffu;
        optical_solid.m_opacity = 0.6f;
        optical_solid.m_surface_flags = kGridSurfaceParticleSolid | kGridSurfaceOpticalBlend;

        const std::array<GridTerrainVisual, 6u> palette{
            solid_a, solid_b, liquid_a, liquid_b, opaque_liquid, optical_solid};
        // A class nibble rides bits 12..15 of the same word; the description
        // selection must read the material from its own row regardless.
        const auto row = [](const u32 material, const u32 class_flags) {
            const auto canonical = lighting::packCell(static_cast<u16>(material), 300.0);
            PPR_TEST_ASSERT(canonical.has_value());
            return (*canonical & 0xfffu) | (class_flags << 12u) | (*canonical & 0xffff'0000u);
        };

        const u32 row_a = row(11u, 0u);
        const u32 row_b = row(12u, 0u);
        const u32 row_liquid_a = row(21u, 2u);
        const u32 row_liquid_b = row(22u, 2u);

        // Two solids in one cell: distinct sampled albedo identity (texture
        // index), tint and roughness, each from its own entry.
        const auto first = selectParticleDescription(palette, row_a, EGridParticlePhase::solid);
        const auto second = selectParticleDescription(palette, row_b, EGridParticlePhase::solid);
        PPR_TEST_ASSERT(first.selected() and second.selected());
        PPR_TEST_ASSERT(first.m_description.m_visual == &palette[0u]);
        PPR_TEST_ASSERT(second.m_description.m_visual == &palette[1u]);
        PPR_TEST_ASSERT(first.m_description.m_phase == EGridParticlePhase::solid);
        PPR_TEST_ASSERT(
            first.m_description.m_visual->m_texture_index != second.m_description.m_visual->m_texture_index and
            first.m_description.m_visual->m_color_rgba != second.m_description.m_visual->m_color_rgba and
            first.m_description.m_visual->m_roughness != second.m_description.m_visual->m_roughness);
        // The canonical packCell word is rebuilt from the row, never a cell word.
        const auto expected_a = lighting::packCell(11u, 300.0);
        PPR_TEST_ASSERT(expected_a.has_value());
        PPR_TEST_ASSERT(first.m_description.m_canonical_word == *expected_a);

        // Two liquids: distinct IOR, absorption and tint identity.
        const auto liquid_first = selectParticleDescription(palette, row_liquid_a, EGridParticlePhase::liquid);
        const auto liquid_second = selectParticleDescription(palette, row_liquid_b, EGridParticlePhase::liquid);
        PPR_TEST_ASSERT(liquid_first.selected() and liquid_second.selected());
        PPR_TEST_ASSERT(liquid_first.m_description.m_visual == &palette[2u]);
        PPR_TEST_ASSERT(liquid_second.m_description.m_visual == &palette[3u]);
        PPR_TEST_ASSERT(
            liquid_first.m_description.m_visual->m_ior != liquid_second.m_description.m_visual->m_ior and
            liquid_first.m_description.m_visual->m_color_rgba != liquid_second.m_description.m_visual->m_color_rgba);
        for (u32 channel = 0u; channel < 3u; ++channel) {
            PPR_TEST_ASSERT(
                liquid_first.m_description.m_visual->m_sigma_per_m[channel] !=
                liquid_second.m_description.m_visual->m_sigma_per_m[channel]);
        }

        // The phase flags choose the description family: a liquid passed to the
        // solid branch is a reject, never a solid shade, and vice versa.
        PPR_TEST_ASSERT(
            selectParticleDescription(palette, row_liquid_a, EGridParticlePhase::solid).m_reject ==
            EGridParticleDescriptionReject::phase_mismatch);
        PPR_TEST_ASSERT(
            selectParticleDescription(palette, row_a, EGridParticlePhase::liquid).m_reject ==
            EGridParticleDescriptionReject::phase_mismatch);

        // The family follows the material's phase, never the blend/draw class.
        // An opaque liquid (liquid phase, no optical-blend bit, so the opaque
        // depth-writing class draws it) still takes the liquid description and is
        // never selected as solid.
        const auto opaque_liquid_liquid = selectParticleDescription(palette, row(23u, 0u), EGridParticlePhase::liquid);
        PPR_TEST_ASSERT(opaque_liquid_liquid.selected());
        PPR_TEST_ASSERT(opaque_liquid_liquid.m_description.m_visual == &palette[4u]);
        PPR_TEST_ASSERT(opaque_liquid_liquid.m_description.m_phase == EGridParticlePhase::liquid);
        PPR_TEST_ASSERT(
            selectParticleDescription(palette, row(23u, 0u), EGridParticlePhase::solid).m_reject ==
            EGridParticleDescriptionReject::phase_mismatch);
        // An optical solid (solid phase with the optical-blend bit, so the
        // blended class draws it) still takes the solid description.
        const auto optical_solid_solid = selectParticleDescription(palette, row(13u, 2u), EGridParticlePhase::solid);
        PPR_TEST_ASSERT(optical_solid_solid.selected());
        PPR_TEST_ASSERT(optical_solid_solid.m_description.m_visual == &palette[5u]);
        PPR_TEST_ASSERT(optical_solid_solid.m_description.m_phase == EGridParticlePhase::solid);
        PPR_TEST_ASSERT(
            selectParticleDescription(palette, row(13u, 2u), EGridParticlePhase::liquid).m_reject ==
            EGridParticleDescriptionReject::phase_mismatch);

        // D9 identity guard: an id the palette does not carry rejects instead of
        // falling back to entry 0 or to another material's description.
        PPR_TEST_ASSERT(
            selectParticleDescription(palette, row(77u, 0u), EGridParticlePhase::solid).m_reject ==
            EGridParticleDescriptionReject::unknown_material);
        PPR_TEST_ASSERT(
            selectParticleDescription(palette, row(0u, 0u), EGridParticlePhase::solid).m_reject ==
            EGridParticleDescriptionReject::unknown_material);
        PPR_TEST_ASSERT(particleMaterialVisual(palette, row(77u, 0u)) == nullptr);
        // Every row resolves through its own id, never a shared entry.
        PPR_TEST_ASSERT(particleMaterialVisual(palette, row_b)->m_material_id == 12u);
        PPR_TEST_ASSERT(particleMaterialVisual(palette, row_liquid_b)->m_material_id == 22u);
    };

    PPR_UNIT_TEST (particle_classes_declare_their_state_and_draw_once_per_class) {
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

        GridTerrainVisual solid{};
        solid.m_material_id = 1u;
        solid.m_color_rgba = 0xff8040ffu;
        solid.m_ior = 1.0f;
        solid.m_roughness = 0.4f;
        solid.m_opacity = 1.0f;
        solid.m_surface_flags = kGridSurfaceParticleSolid;

        GridTerrainVisual liquid{};
        liquid.m_material_id = 2u;
        liquid.m_color_rgba = 0x2040ffffu;
        liquid.m_ior = 1.333f;
        liquid.m_roughness = 0.1f;
        liquid.m_opacity = 0.6f;
        liquid.m_refraction_depth_m = 0.2f;
        liquid.m_surface_flags = kGridSurfaceParticleLiquid | kGridSurfaceOpticalBlend;

        // The magma-like case: liquid phase without the optical-blend bit, so the
        // packer routes it to the opaque (depth-writing) class while the
        // description family stays liquid.
        GridTerrainVisual opaque_liquid = liquid;
        opaque_liquid.m_material_id = 3u;
        opaque_liquid.m_color_rgba = 0xff2000ffu;
        opaque_liquid.m_ior = 1.2f;
        opaque_liquid.m_opacity = 1.0f;
        opaque_liquid.m_refraction_depth_m = 0.0f;
        opaque_liquid.m_surface_flags = kGridSurfaceParticleLiquid;

        // One solid row, one optical liquid row, one opaque liquid row and one
        // gas row over the same ids and indirect arguments. Every published
        // material must exist in the palette: admission already enforces that
        // identity rule, and the shader's D9 guard is the second line of defence.
        std::array<GridParticleMetadataRow, 4u> rows{};
        for (u32 row = 0u; row < 3u; ++row) {
            const auto cell = lighting::packCell(static_cast<u16>(row + 1u), 300.0);
            PPR_TEST_ASSERT(cell.has_value());
            rows[row].m_id_low = row + 1u;
            rows[row].m_source_row = row;
            rows[row].m_material_temperature = *cell;
            rows[row].m_radius = 0.5f;
        }
        const auto gas_cell = lighting::packCell(1u, 300.0);
        PPR_TEST_ASSERT(gas_cell.has_value());
        rows[3u].m_id_low = 4u;
        rows[3u].m_source_row = 0xffff'ffffu;
        rows[3u].m_material_temperature = *gas_cell;
        rows[3u].m_gas_x = 1.5f;
        rows[3u].m_gas_y = 2.5f;
        rows[3u].m_radius = 0.5f;
        const auto metadata = prepareGridParticleMetadata(fixture.device(), rows, 3u, 1.0, 1u);
        PPR_TEST_ASSERT(metadata.has_value());

        // Three moving rows: hot position + enabled in native cells (the packer
        // subtracts the view's 3-cell origin), cold volume -> radius 1 m.
        std::array<std::array<float, 16u>, 3u> hot{};
        hot[0u][0u] = 4.5f;
        hot[0u][1u] = 5.5f;
        hot[0u][15u] = 1.0f;
        hot[1u][0u] = 6.5f;
        hot[1u][1u] = 5.5f;
        hot[1u][15u] = 1.0f;
        hot[2u][0u] = 8.5f;
        hot[2u][1u] = 5.5f;
        hot[2u][15u] = 1.0f;
        std::array<std::array<float, 6u>, 3u> cold{};
        cold[0u][1u] = std::numbers::pi_v<float>;
        cold[1u][1u] = std::numbers::pi_v<float>;
        cold[2u][1u] = std::numbers::pi_v<float>;

        GridParticleGpuView view{};
        view.m_numerical = makeBuffer(fixture.device(), sizeof(hot), kGridParticleNumericalStride, hot.data());
        view.m_readonly = makeBuffer(fixture.device(), sizeof(cold), kGridParticleReadonlyStride, cold.data());
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
        stage.m_terrain_visuals = {solid, liquid, opaque_liquid};
        stage.m_tiles.push_back(
            GridTileSubmission{.m_dirty_mask = 1u, .m_tile_range = {0, 0, 128, 128}, .m_chunk_id = 0u});
        stage.m_gpu_particles = view;
        PPR_TEST_ASSERT(not grid.applyStagedFrame(stage));
        PPR_TEST_ASSERT(not grid.applyGpuParticleView(view));

        CameraModel model{};
        model.m_camera_mode = ECameraProjection::orthographic;
        model.m_origin = float3{-0.5f, 0.0f, -0.5f};
        model.m_ortho_scale = 8.0f;
        Camera camera{ECameraProjection::orthographic};
        camera.updateModel(TimeSpan{}, model, Viewport{PixelRect{0, 0, 64, 64}});
        const auto snapshot = camera.getSnapshot();

        // The class states are only meaningful with a depth attachment, so the
        // pass supplies the D32Float surface the signature names.
        rhi::TextureDesc target_description{};
        target_description.type = rhi::TextureType::Texture2D;
        target_description.size = {.width = 64u, .height = 64u, .depth = 1u};
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

        rhi::TextureDesc depth_description = target_description;
        depth_description.format = rhi::Format::D32Float;
        depth_description.usage = rhi::TextureUsage::DepthStencil;
        depth_description.defaultState = rhi::ResourceState::DepthWrite;
        rhi::ComPtr<rhi::ITexture> depth{};
        rhi::ComPtr<rhi::ITextureView> depth_view{};
        PPR_TEST_ASSERT(
            not make_error_code(fixture.device().createTexture(depth_description, nullptr, depth.writeRef())));
        PPR_TEST_ASSERT(not make_error_code(depth->getDefaultView(depth_view.writeRef())));

        const auto viewport = rhi::Viewport::fromSize(64.0f, 64.0f);
        const auto scissor = rhi::ScissorRect::fromSize(64u, 64u);
        PPR_TEST_ASSERT(not grid.update(TimeSpan{}, snapshot));
        const auto prepare = [&](const PreRenderContext &context) -> std::error_code {
            return grid.prepareGpuParticles(context);
        };
        // The frame draws like the scene families: the legacy tile first, then
        // the restored particle classes. The terrain draw also creates the cell
        // atlas/dummy the surface bindings require (a particle-only frame would
        // bind a null diagnostic buffer).
        // Measure the particle classes exactly: the legacy CPU-staged tile draw
        // does not bump m_draw_commands (only the GPU-visibility/indirect and
        // particle lanes do), so the delta below is the per-class count.
        u64 particle_draws_before{};
        u64 particle_draws_after{};
        const auto draw = [&](const DrawContext &context) -> std::error_code {
            if (const auto ec = grid.render(context)) {
                return ec;
            }
            particle_draws_before = grid.observations().m_draw_commands;
            if (const auto ec = grid.renderSolidLiquidParticles(context)) {
                return ec;
            }
            if (const auto ec = grid.renderGasParticles(context)) {
                return ec;
            }
            particle_draws_after = grid.observations().m_draw_commands;
            return {};
        };

        DrawSubmission submission{"grid particle classes", DrawCallback{draw}};
        submission.m_viewport = viewport;
        submission.m_scissor = scissor;
        rhi::ComPtr<rhi::ITextureView> target_view{};
        PPR_TEST_ASSERT(not make_error_code(target->getDefaultView(target_view.writeRef())));
        rhi::RenderPassColorAttachment color{};
        color.view = target_view.get();
        color.clearValue[3] = 1.0f;
        rhi::RenderPassDepthStencilAttachment depth_attachment{};
        depth_attachment.view = depth_view.get();
        depth_attachment.depthClearValue = 1.0f;
        rhi::RenderPassDesc pass_description{};
        pass_description.colorAttachments = &color;
        pass_description.colorAttachmentCount = 1u;
        pass_description.depthStencilAttachment = &depth_attachment;

        const auto before = grid.observations();
        PPR_TEST_ASSERT(not renderer.render(
            string_literal{std::in_place, "grid particle classes"}, pass_description, {submission},
            PreRenderCallback{prepare}));
        grid.notifyFrameSubmitted(renderer.lastSubmittedFenceValue());
        PPR_TEST_ASSERT(not renderer.waitOnHost());
        PPR_TEST_ASSERT(not renderer.retireCompletedFrames());
        const auto after = grid.observations();
        // One draw per particle class over the same prepared ids/arguments: the
        // opaque class (solid and opaque-liquid rows), the blended class and the
        // gas lane.
        PPR_TEST_ASSERT(particle_draws_after == particle_draws_before + 3u);
        // Every published material is in the palette, so the identity guard has
        // nothing to count here; an absent id is covered by the selection
        // helper's `unknown_material` case.
        PPR_TEST_ASSERT(after.m_particle_description_rejections == 0u);

        // The packed class follows kSurfaceOpticalBlend only, and the description
        // family follows the phase: the opaque liquid row is packed into the
        // opaque class (the pipeline that draws it) while its family is liquid,
        // and the optical liquid row is packed into the blended class.
        std::array<ParticleGpu, 4u> packed{};
        const auto particle_buffers = grid.particleBuffersForTest();
        PPR_TEST_ASSERT(not make_error_code(
            fixture.device().readBuffer(particle_buffers[0u], 0u, packed.size() * sizeof(ParticleGpu), packed.data())));
        const auto class_of = [](const ParticleGpu &row) { return (row.m_material_flags_temperature >> 12u) & 0xfu; };
        PPR_TEST_ASSERT(class_of(packed[0u]) == 0u and class_of(packed[2u]) == 0u);
        PPR_TEST_ASSERT(class_of(packed[1u]) == 2u);
        PPR_TEST_ASSERT(class_of(packed[3u]) == 1u);
        PPR_TEST_ASSERT((packed[2u].m_material_flags_temperature & 0xfffu) == 3u);
        const auto opaque_liquid_description = selectParticleDescription(
            stage.m_terrain_visuals, packed[2u].m_material_flags_temperature, EGridParticlePhase::liquid);
        PPR_TEST_ASSERT(
            opaque_liquid_description.selected() and
            opaque_liquid_description.m_description.m_phase == EGridParticlePhase::liquid);

        auto *const gas = grid.particlePipelineForTest(EGridParticleDrawClass::gas);
        auto *const opaque = grid.particlePipelineForTest(EGridParticleDrawClass::opaque);
        auto *const optical = grid.particlePipelineForTest(EGridParticleDrawClass::optical);
        PPR_TEST_ASSERT(gas != nullptr and opaque != nullptr and optical != nullptr);
        PPR_TEST_ASSERT(gas != opaque and opaque != optical and gas != optical);

        // Gas keeps the retained depth-disabled lane. This fixture renders the
        // x1 entry, whose fragment writes straight alpha, so its source factor
        // is SrcAlpha; the sample-rate entry premultiplies and uses One.
        const rhi::RenderPipelineDesc &gas_desc = gas->getDesc();
        PPR_TEST_ASSERT(not gas_desc.depthStencil.depthTestEnable and not gas_desc.depthStencil.depthWriteEnable);
        PPR_TEST_ASSERT(gas_desc.targets[0u].enableBlend);
        PPR_TEST_ASSERT(gas_desc.targets[0u].color.srcFactor == rhi::BlendFactor::SrcAlpha);
        PPR_TEST_ASSERT(gas_desc.targets[0u].color.dstFactor == rhi::BlendFactor::InvSrcAlpha);

        // Opaque: LessEqual, depth writes on, premultiplied blending, so its
        // analytic edge coverage composites like the terrain pipeline's.
        const rhi::RenderPipelineDesc &opaque_desc = opaque->getDesc();
        PPR_TEST_ASSERT(opaque_desc.depthStencil.depthTestEnable and opaque_desc.depthStencil.depthWriteEnable);
        PPR_TEST_ASSERT(opaque_desc.depthStencil.depthFunc == rhi::ComparisonFunc::LessEqual);
        PPR_TEST_ASSERT(opaque_desc.targets[0u].enableBlend);
        PPR_TEST_ASSERT(opaque_desc.targets[0u].color.srcFactor == rhi::BlendFactor::One);
        PPR_TEST_ASSERT(opaque_desc.targets[0u].color.dstFactor == rhi::BlendFactor::InvSrcAlpha);

        // Optical: LessEqual, no depth writes, premultiplied blending.
        const rhi::RenderPipelineDesc &optical_desc = optical->getDesc();
        PPR_TEST_ASSERT(optical_desc.depthStencil.depthTestEnable and not optical_desc.depthStencil.depthWriteEnable);
        PPR_TEST_ASSERT(optical_desc.depthStencil.depthFunc == rhi::ComparisonFunc::LessEqual);
        PPR_TEST_ASSERT(optical_desc.targets[0u].enableBlend);
        PPR_TEST_ASSERT(optical_desc.targets[0u].color.srcFactor == rhi::BlendFactor::One);
        PPR_TEST_ASSERT(optical_desc.targets[0u].color.dstFactor == rhi::BlendFactor::InvSrcAlpha);
        PPR_TEST_ASSERT(
            opaque_desc.multisample.sampleCount == 1u and
            optical_desc.multisample.sampleCount == 1u and
            gas_desc.multisample.sampleCount == 1u);

        // Declaration order of the per-draw selector both particle stages read:
        // pass, particle_class, surface_phase_mask, pad2. Field 2 was the single
        // admitted lane material before the phase/class cutover; it is the phase
        // mask now, so a material-id compare cannot silently reappear.
        shader::SharedModule module{};
        PPR_TEST_ASSERT(not fixture.m_shaders->loadModuleFromFile(
            std::filesystem::current_path() / "shaders" / "grid_tiles.slang", "grid_tiles", module.writeRef()));
        auto *const layout = module->getLayout();
        PPR_TEST_ASSERT(layout != nullptr);
        slang::TypeLayoutReflection *switch_layout = nullptr;
        for (unsigned index = 0u; index < layout->getParameterCount(); ++index) {
            auto *const parameter = layout->getParameterByIndex(index);
            if (std::string_view{parameter->getName()} == "g_grid_pass") {
                switch_layout = parameter->getTypeLayout()->getElementTypeLayout();
                break;
            }
        }

        PPR_TEST_ASSERT(switch_layout != nullptr);
        PPR_TEST_ASSERT(switch_layout->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM) == 16u);
        constexpr std::array<std::pair<const char *, u32>, 4u> switch_fields{{
            {"pass", 0u},
            {"particle_class", 4u},
            {"surface_phase_mask", 8u},
            {"pad2", 12u},
        }};
        PPR_TEST_ASSERT(switch_layout->getFieldCount() == static_cast<unsigned>(switch_fields.size()));
        for (u32 index = 0u; index < switch_fields.size(); ++index) {
            auto *const field = switch_layout->getFieldByIndex(index);
            PPR_TEST_ASSERT(field != nullptr);
            PPR_TEST_ASSERT(std::string_view{field->getName()} == switch_fields[index].first);
            PPR_TEST_ASSERT(field->getOffset(SLANG_PARAMETER_CATEGORY_UNIFORM) == switch_fields[index].second);
            PPR_TEST_ASSERT(field->getTypeLayout()->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM) == 4u);
        }

        // The lane constant buffer the selection, accumulation and composite
        // entries share. Its field order is the ABI the three pipelines read, so
        // the test pins the same names and offsets the production reflection
        // check enforces instead of trusting only the shader-side compiler.
        slang::TypeLayoutReflection *lane_layout = nullptr;
        for (unsigned index = 0u; index < layout->getParameterCount(); ++index) {
            auto *const parameter = layout->getParameterByIndex(index);
            if (std::string_view{parameter->getName()} == "g_particle_surface") {
                lane_layout = parameter->getTypeLayout()->getElementTypeLayout();
                break;
            }
        }

        PPR_TEST_ASSERT(lane_layout != nullptr);
        PPR_TEST_ASSERT(lane_layout->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM) == 16u);
        constexpr std::array<std::pair<const char *, u32>, 4u> lane_fields{{
            {"m_pixel_world", 0u},
            {"m_phase_mask", 4u},
            {"m_composite_class", 8u},
            {"m_pad", 12u},
        }};
        PPR_TEST_ASSERT(lane_layout->getFieldCount() == static_cast<unsigned>(lane_fields.size()));
        for (u32 index = 0u; index < lane_fields.size(); ++index) {
            auto *const field = lane_layout->getFieldByIndex(index);
            PPR_TEST_ASSERT(field != nullptr);
            PPR_TEST_ASSERT(std::string_view{field->getName()} == lane_fields[index].first);
            PPR_TEST_ASSERT(field->getOffset(SLANG_PARAMETER_CATEGORY_UNIFORM) == lane_fields[index].second);
        }

        // The smoothing pass's second constant block: the frame's own zoom, the
        // clamped halo and the field extent. It is a scalar-only block (no
        // fixed-size array), so its layout is exactly eight 4-byte words.
        slang::TypeLayoutReflection *filter_layout = nullptr;
        for (unsigned index = 0u; index < layout->getParameterCount(); ++index) {
            auto *const parameter = layout->getParameterByIndex(index);
            if (std::string_view{parameter->getName()} == "g_particle_filter") {
                filter_layout = parameter->getTypeLayout()->getElementTypeLayout();
                break;
            }
        }

        PPR_TEST_ASSERT(filter_layout != nullptr);
        PPR_TEST_ASSERT(filter_layout->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM) == 32u);
        constexpr std::array<std::pair<const char *, u32>, 8u> filter_fields{{
            {"m_samples_per_cell", 0u},
            {"m_sigma_cell", 4u},
            {"m_halo", 8u},
            {"m_pad", 12u},
            {"m_extent_x", 16u},
            {"m_extent_y", 20u},
            {"m_pad2", 24u},
            {"m_pad3", 28u},
        }};
        PPR_TEST_ASSERT(filter_layout->getFieldCount() == static_cast<unsigned>(filter_fields.size()));
        for (u32 index = 0u; index < filter_fields.size(); ++index) {
            auto *const field = filter_layout->getFieldByIndex(index);
            PPR_TEST_ASSERT(field != nullptr);
            PPR_TEST_ASSERT(std::string_view{field->getName()} == filter_fields[index].first);
            PPR_TEST_ASSERT(field->getOffset(SLANG_PARAMETER_CATEGORY_UNIFORM) == filter_fields[index].second);
        }

        // The contour cues now read one shared stored axis: the prepare pass owns
        // g_orientation_previous/g_orientation_current and the draw owns
        // g_particle_orientation. The Q16 fill grid is no longer a draw input, so
        // the draw declares the winner grid only; no per-instance field was added
        // to the 24-byte row.
        u32 best_row_bindings = 0u;
        u32 cell_fill_bindings = 0u;
        u32 orientation_bindings = 0u;
        for (unsigned index = 0u; index < layout->getParameterCount(); ++index) {
            const std::string_view name{layout->getParameterByIndex(index)->getName()};
            if (name == "g_best_row")
                ++best_row_bindings;
            if (name == "g_cell_fill")
                ++cell_fill_bindings;
            if (name == "g_particle_orientation")
                ++orientation_bindings;

            PPR_TEST_ASSERT(name.find("tangent") == std::string_view::npos);
        }

        PPR_TEST_ASSERT(best_row_bindings >= 1u);
        PPR_TEST_ASSERT(cell_fill_bindings == 0u);
        PPR_TEST_ASSERT(orientation_bindings == 1u);

        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL particle_classes draws={} rejections={} opaque_depth=({},{}) "
                "optical_depth=({},{})",
                after.m_draw_commands - before.m_draw_commands, after.m_particle_description_rejections,
                opaque_desc.depthStencil.depthTestEnable, opaque_desc.depthStencil.depthWriteEnable,
                optical_desc.depthStencil.depthTestEnable, optical_desc.depthStencil.depthWriteEnable));
    };

    PPR_UNIT_TEST (particle_orientation_publication_is_transactional_and_growth_preserves_survivors) {
        // Real GridPass/frame path, no pixels: one birth publication, one repeated
        // publication (bit-identical accepted bytes, no update), a converged ramp,
        // and a capacity growth that must preserve the survivor rows while the
        // appended identity starts its own history.
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

        GridTerrainVisual solid{};
        solid.m_material_id = 1u;
        solid.m_color_rgba = 0xff8040ffu;
        solid.m_ior = 1.0f;
        solid.m_roughness = 0.4f;
        solid.m_opacity = 1.0f;
        solid.m_surface_flags = kGridSurfaceParticleSolid;

        const auto makeMetadata = [&](const u32 count, const u64 version) {
            Array<GridParticleMetadataRow> rows(count);
            for (u32 row = 0u; row < count; ++row) {
                const auto cell = lighting::packCell(1u, 300.0);
                PPR_TEST_ASSERT(cell.has_value());
                rows[row].m_id_low = row + 1u;
                rows[row].m_source_row = row;
                rows[row].m_material_temperature = *cell;
                rows[row].m_radius = 0.5f;
            }

            auto metadata = prepareGridParticleMetadata(
                fixture.device(), std::span<const GridParticleMetadataRow>{rows.data(), rows.size()}, count, 1.0,
                version);
            PPR_TEST_ASSERT(metadata.has_value());
            return *metadata;
        };

        // A constant non-zero published displacement gives a non-zero flow target
        // even with no lighting ownership binding (the own-row fallback), so the
        // amplitude ramp has something to converge to and survivors can be compared
        // bit for bit.
        const auto makeHot = [](const u32 count) {
            Array<std::array<float, 16u>> hot(count);
            for (u32 row = 0u; row < count; ++row) {
                hot[row][0u] = 4.5f + 2.0f * static_cast<float>(row);
                hot[row][1u] = 5.5f;
                hot[row][2u] = 0.03f;
                hot[row][3u] = 0.04f;
                hot[row][15u] = 1.0f;
            }

            return hot;
        };
        const auto makeCold = [](const u32 count) {
            Array<std::array<float, 6u>> cold(count);
            for (auto &row: cold)
                row[1u] = std::numbers::pi_v<float>;

            return cold;
        };
        const auto makeView = [&](const std::shared_ptr<const GridParticleMetadataResource> &metadata,
                                  const Array<std::array<float, 16u>> &hot, const Array<std::array<float, 6u>> &cold,
                                  const u64 epoch) {
            GridParticleGpuView view{};
            view.m_numerical = makeBuffer(
                fixture.device(), hot.size() * sizeof(hot[0u]), kGridParticleNumericalStride, hot.data());
            view.m_readonly = makeBuffer(
                fixture.device(), cold.size() * sizeof(cold[0u]), kGridParticleReadonlyStride, cold.data());
            view.m_metadata = metadata;
            view.m_live_epoch = std::make_shared<std::atomic<u64>>(epoch);
            view.m_world = grid.activeWorldIdentity();
            view.m_layout = kGridParticleNumericalLayout;
            view.m_stride = kGridParticleNumericalStride;
            view.m_readonly_stride = kGridParticleReadonlyStride;
            view.m_origin = 3.0;
            view.m_device_epoch = epoch;
            view.m_publication = epoch;
            view.m_terrain_revision = 1u;
            view.m_joint_tick = epoch;
            return view;
        };

        auto hot = makeHot(2u);
        auto cold = makeCold(2u);
        auto metadata = makeMetadata(2u, 1u);
        auto view = makeView(metadata, hot, cold, 1u);
        PPR_TEST_ASSERT(view.current());

        GridStagedFrame stage{};
        stage.m_world_edge = WorldLightingPass::kWorldEdge;
        stage.m_cell_size_m = 1.0f;
        stage.m_terrain_visuals = {solid};
        stage.m_tiles.push_back(
            GridTileSubmission{.m_dirty_mask = 1u, .m_tile_range = {0, 0, 128, 128}, .m_chunk_id = 0u});
        stage.m_gpu_particles = view;
        PPR_TEST_ASSERT(not grid.applyStagedFrame(stage));
        PPR_TEST_ASSERT(not grid.applyGpuParticleView(view));

        CameraModel model{};
        model.m_camera_mode = ECameraProjection::orthographic;
        model.m_origin = float3{-0.5f, 0.0f, -0.5f};
        model.m_ortho_scale = 8.0f;
        Camera camera{ECameraProjection::orthographic};
        camera.updateModel(TimeSpan{}, model, Viewport{PixelRect{0, 0, 64, 64}});
        const auto snapshot = camera.getSnapshot();
        PPR_TEST_ASSERT(not grid.update(TimeSpan{}, snapshot));

        rhi::TextureDesc target_description{};
        target_description.type = rhi::TextureType::Texture2D;
        target_description.size = {.width = 64u, .height = 64u, .depth = 1u};
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

        rhi::TextureDesc depth_description = target_description;
        depth_description.format = rhi::Format::D32Float;
        depth_description.usage = rhi::TextureUsage::DepthStencil;
        depth_description.defaultState = rhi::ResourceState::DepthWrite;
        rhi::ComPtr<rhi::ITexture> depth{};
        rhi::ComPtr<rhi::ITextureView> depth_view{};
        PPR_TEST_ASSERT(
            not make_error_code(fixture.device().createTexture(depth_description, nullptr, depth.writeRef())));
        PPR_TEST_ASSERT(not make_error_code(depth->getDefaultView(depth_view.writeRef())));

        const auto viewport = rhi::Viewport::fromSize(64.0f, 64.0f);
        const auto scissor = rhi::ScissorRect::fromSize(64u, 64u);
        const auto prepare = [&](const PreRenderContext &context) -> std::error_code {
            return grid.prepareGpuParticles(context);
        };
        const auto draw = [&](const DrawContext &context) -> std::error_code {
            PPR_RETURN_ERROR_ON_FAIL(GridPass, grid.render(context));
            PPR_RETURN_ERROR_ON_FAIL(GridPass, grid.renderSolidLiquidParticles(context));
            return grid.renderGasParticles(context);
        };
        DrawSubmission submission{"grid particle orientation", DrawCallback{draw}};
        submission.m_viewport = viewport;
        submission.m_scissor = scissor;
        rhi::ComPtr<rhi::ITextureView> target_view{};
        PPR_TEST_ASSERT(not make_error_code(target->getDefaultView(target_view.writeRef())));
        rhi::RenderPassColorAttachment color{};
        color.view = target_view.get();
        color.clearValue[3] = 1.0f;
        rhi::RenderPassDepthStencilAttachment depth_attachment{};
        depth_attachment.view = depth_view.get();
        depth_attachment.depthClearValue = 1.0f;
        rhi::RenderPassDesc pass_description{};
        pass_description.colorAttachments = &color;
        pass_description.colorAttachmentCount = 1u;
        pass_description.depthStencilAttachment = &depth_attachment;

        const auto encode = [&]() -> std::error_code {
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, renderer.render(
                              string_literal{std::in_place, "grid particle orientation"}, pass_description,
                              {submission}, PreRenderCallback{prepare}));
            grid.notifyFrameSubmitted(renderer.lastSubmittedFenceValue());
            PPR_RETURN_ERROR_ON_FAIL(GridPass, renderer.waitOnHost());
            return renderer.retireCompletedFrames();
        };
        const auto readAccepted = [&](const u32 count) {
            Array<ParticleOrientationGpu> rows(count);
            rhi::IBuffer *const accepted = grid.particleOrientationAcceptedForTest();
            PPR_TEST_ASSERT(accepted != nullptr);
            PPR_TEST_ASSERT(not make_error_code(
                fixture.device().readBuffer(accepted, 0u, rows.size() * sizeof(ParticleOrientationGpu), rows.data())));
            return rows;
        };

        const auto sameRows = [](const auto &left, const auto &right, const u32 count) {
            return count != 0u and std::memcmp(left.data(), right.data(), count * sizeof(ParticleOrientationGpu)) == 0;
        };

        // Birth publication: the first accepted publication initialises the target
        // axis and then applies exactly one clamped amplitude step, so the ramp is
        // observable rather than instantaneous.
        PPR_TEST_ASSERT(not encode());
        PPR_TEST_ASSERT(grid.particleOrientationValidForTest());
        const auto born = readAccepted(2u);
        PPR_TEST_ASSERT(((born[0u].m_angle_bias_stretch >> 16u) & 0xffu) == 0u);
        PPR_TEST_ASSERT(((born[0u].m_angle_bias_stretch >> 24u) & 0xffu) == 32u);

        // A3: the orientation clock is the accepted presentation identity, not the
        // pack token. A publication/joint-tick-only accepted change advances the
        // ramp with no redundant particle pack.
        const u64 pack_before_identity = grid.particlePackEncodesForTest();
        auto identity_view = view;
        identity_view.m_publication = 2u;
        identity_view.m_joint_tick = 2u;
        PPR_TEST_ASSERT(not grid.applyGpuParticleView(identity_view));
        PPR_TEST_ASSERT(not encode());
        PPR_TEST_ASSERT(grid.particlePackEncodesForTest() == pack_before_identity);
        const auto identity_updated = readAccepted(2u);
        PPR_TEST_ASSERT(((identity_updated[0u].m_angle_bias_stretch >> 24u) & 0xffu) == 64u);
        PPR_TEST_ASSERT(not sameRows(identity_updated, born, 2u));

        // Repeated publication with the same identity: no pack, no update, and the
        // accepted history is bit-identical.
        const u64 pack_encodes = grid.particlePackEncodesForTest();
        PPR_TEST_ASSERT(not encode());
        PPR_TEST_ASSERT(grid.particlePackEncodesForTest() == pack_encodes);
        const auto repeated = readAccepted(2u);
        PPR_TEST_ASSERT(sameRows(repeated, identity_updated, 2u));

        // Converge the amplitude ramp with new accepted publications.
        for (u32 publish = 0u; publish < 10u; ++publish) {
            const u64 epoch = 10u + publish;
            auto next = view;
            next.m_live_epoch = std::make_shared<std::atomic<u64>>(epoch);
            next.m_device_epoch = epoch;
            next.m_publication = epoch;
            next.m_joint_tick = epoch;
            PPR_TEST_ASSERT(next.current());
            PPR_TEST_ASSERT(not grid.applyGpuParticleView(next));
            PPR_TEST_ASSERT(not encode());
        }

        const auto converged = readAccepted(2u);
        PPR_TEST_ASSERT(((converged[0u].m_angle_bias_stretch >> 16u) & 0xffu) == 0u);
        PPR_TEST_ASSERT(((converged[0u].m_angle_bias_stretch >> 24u) & 0xffu) >= 200u);

        // Growth allocates the replacement roles without an old-to-new copy. The
        // retained accepted allocation stays the readable history, and because the
        // converged state is a fixed point the surviving IDs keep their rows.
        auto grown_hot = makeHot(3u);
        auto grown_cold = makeCold(3u);
        auto grown_metadata = makeMetadata(3u, 2u);
        auto grown_view = makeView(grown_metadata, grown_hot, grown_cold, 32u);
        PPR_TEST_ASSERT(not grid.applyGpuParticleView(grown_view));
        PPR_TEST_ASSERT(not encode());
        const auto grown = readAccepted(3u);
        PPR_TEST_ASSERT(sameRows(grown, converged, 2u));
        PPR_TEST_ASSERT(((grown[2u].m_angle_bias_stretch >> 16u) & 0xffu) == 0u);
        PPR_TEST_ASSERT(((grown[2u].m_angle_bias_stretch >> 24u) & 0xffu) == 32u);

        // A1/A2: a prepared growth that is definitely not submitted must leave the
        // retained history accepted and readable, and the fresh roles must never be
        // drawn. Then a revoked-source hold and the retry must not read uninitialised
        // bytes.
        const auto before_rejected_growth = readAccepted(3u);
        const u32 role_before_rejected_growth = grid.particleOrientationAcceptedRoleForTest();
        auto next_hot = makeHot(4u);
        auto next_cold = makeCold(4u);
        auto next_metadata = makeMetadata(4u, 3u);
        auto next_view = makeView(next_metadata, next_hot, next_cold, 40u);
        PPR_TEST_ASSERT(not grid.applyGpuParticleView(next_view));
        {
            rhi::ComPtr<rhi::ICommandEncoder> encoder{};
            PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(encoder.writeRef())));
            FenceWaitHook wait{[&](const FenceCompletionValue value) { return renderer.waitForFenceValue(value); }};
            PreRenderContext context{fixture.device(), *encoder, renderer.completedFenceValue(), std::move(wait)};
            PPR_TEST_ASSERT(not grid.prepareGpuParticles(context));
            // The candidate was encoded but this frame is definitely unsubmitted.
            grid.discardUnsubmittedParticles();
        }

        const auto roles = grid.particleOrientationRolesForTest();
        rhi::IBuffer *const retained_history = grid.particleOrientationHistoryForTest();
        PPR_TEST_ASSERT(retained_history != nullptr);
        PPR_TEST_ASSERT(retained_history != roles[0u] and retained_history != roles[1u]);
        PPR_TEST_ASSERT(grid.particleOrientationAcceptedRoleForTest() == role_before_rejected_growth);
        PPR_TEST_ASSERT(sameRows(readAccepted(3u), before_rejected_growth, 3u));
        PPR_TEST_ASSERT(grid.particleOrientationDrawForTest() == retained_history);

        // Revoke the source: the next frame is a hold and must draw the retained
        // history, never the rejected candidate.
        next_view.m_live_epoch = std::make_shared<std::atomic<u64>>(next_view.m_device_epoch + 1u);
        PPR_TEST_ASSERT(not grid.applyGpuParticleView(next_view));
        PPR_TEST_ASSERT(not encode());
        PPR_TEST_ASSERT(grid.particlePresentationForTest() == EParticlePresentation::held);
        PPR_TEST_ASSERT(grid.particleOrientationDrawForTest() == retained_history);
        PPR_TEST_ASSERT(sameRows(readAccepted(3u), before_rejected_growth, 3u));

        // Retry the same 4-row publication: it grows nothing, re-initialises the
        // appended identity and continues the survivors from the retained history.
        auto retry_view = next_view;
        retry_view.m_live_epoch = std::make_shared<std::atomic<u64>>(41u);
        retry_view.m_device_epoch = 41u;
        retry_view.m_publication = 41u;
        retry_view.m_joint_tick = 41u;
        PPR_TEST_ASSERT(retry_view.current());
        PPR_TEST_ASSERT(not grid.applyGpuParticleView(retry_view));
        PPR_TEST_ASSERT(not encode());
        PPR_TEST_ASSERT(grid.particleOrientationValidForTest());
        PPR_TEST_ASSERT(grid.particleOrientationCountForTest() == 4u);
        const auto retried = readAccepted(4u);
        PPR_TEST_ASSERT(sameRows(retried, before_rejected_growth, 3u));
        PPR_TEST_ASSERT(((retried[3u].m_angle_bias_stretch >> 16u) & 0xffu) == 0u);
        PPR_TEST_ASSERT(((retried[3u].m_angle_bias_stretch >> 24u) & 0xffu) == 32u);

        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL particle_orientation birth_stretch={} identity_stretch={} "
                "converged_stretch={} appended_stretch={} retried_stretch={}",
                (born[0u].m_angle_bias_stretch >> 24u) & 0xffu,
                (identity_updated[0u].m_angle_bias_stretch >> 24u) & 0xffu,
                (converged[0u].m_angle_bias_stretch >> 24u) & 0xffu, (grown[2u].m_angle_bias_stretch >> 24u) & 0xffu,
                (retried[3u].m_angle_bias_stretch >> 24u) & 0xffu));
    };

    PPR_UNIT_TEST (held_publication_reculls_retained_rows_against_the_current_camera) {
        // The peer review's clarification 1: a held frame must rerasterise the
        // retained rows against the current camera. A pan that leaves a row's
        // frustum culls it, and a pan that brings another row in reveals it, while
        // the retained publication identity, the packed rows and the canonical cue
        // history stay untouched. An unchanged camera reuses the retained
        // compaction bit for bit and encodes no dispatch.
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

        GridTerrainVisual solid{};
        solid.m_material_id = 1u;
        solid.m_color_rgba = 0xff8040ffu;
        solid.m_ior = 1.0f;
        solid.m_roughness = 0.4f;
        solid.m_opacity = 1.0f;
        solid.m_surface_flags = kGridSurfaceParticleSolid;

        // Two rows 8 m apart on the z = 0 plane, small enough that the 3.0625 r
        // cull margin cannot keep both inside a narrow orthographic view.
        constexpr float particle_radius_m = 0.05f;
        constexpr float left_x_m = 1.0f;
        constexpr float right_x_m = 9.0f;
        constexpr float origin_cells = 3.0f;

        const auto makeMetadata = [&](const u32 count, const u64 version) {
            Array<GridParticleMetadataRow> rows(count);
            for (u32 row = 0u; row < count; ++row) {
                const auto cell = lighting::packCell(1u, 300.0);
                PPR_TEST_ASSERT(cell.has_value());
                rows[row].m_id_low = row + 1u;
                rows[row].m_source_row = row;
                rows[row].m_material_temperature = *cell;
                rows[row].m_radius = particle_radius_m;
            }

            auto metadata = prepareGridParticleMetadata(
                fixture.device(), std::span<const GridParticleMetadataRow>{rows.data(), rows.size()}, count, 1.0,
                version);
            PPR_TEST_ASSERT(metadata.has_value());
            return *metadata;
        };

        // Distinct per-row cues. The stored angle is the UNORIENTED half-angle of the
        // doubled motion axis, so it is quadratic in the motion: flipping both motion
        // components maps to the same axis and the same conic. Opposite-signed cross
        // terms therefore require opposite signs of motion.x * motion.y (quadrants
        // I/III versus II/IV), not a direction flip. For these cues, with the birth
        // slew landing on stretch 32/255 (bias 0, so eta = 1):
        //   row 0  motion ( 0.03,  0.04) -> phi = 0.9273 -> b = -0.055006,
        //                                   a = 0.958745, c = 0.926659
        //   row 1  motion ( 0.04, -0.03) -> phi = 2.4981 -> b = +0.055006,
        //                                   a = 0.926659, c = 0.958745
        // Equal |b| and identical eigenvalues: only a signed coefficient comparison
        // can tell these records apart.
        constexpr std::array<std::array<float, 2u>, 2u> motions{{{0.03f, 0.04f}, {0.04f, -0.03f}}};
        const auto makeHot = [&](const std::array<float, 2u> positions_x_m) {
            Array<std::array<float, 16u>> hot(positions_x_m.size());
            for (std::size_t row = 0u; row < hot.size(); ++row) {
                hot[row][0u] = positions_x_m[row] + origin_cells;
                hot[row][1u] = 1.0f + origin_cells;
                hot[row][2u] = motions[row][0u];
                hot[row][3u] = motions[row][1u];
                hot[row][15u] = 1.0f;
            }

            return hot;
        };
        const auto makeCold = [&](const u32 count) {
            Array<std::array<float, 6u>> cold(count);
            for (auto &row: cold) {
                row[1u] = std::numbers::pi_v<float> * particle_radius_m * particle_radius_m;
            }

            return cold;
        };
        const auto makeView = [&](const std::shared_ptr<const GridParticleMetadataResource> &metadata,
                                  const Array<std::array<float, 16u>> &hot, const Array<std::array<float, 6u>> &cold,
                                  const std::shared_ptr<std::atomic<u64>> &live_epoch, const u64 epoch) {
            GridParticleGpuView view{};
            view.m_numerical = makeBuffer(
                fixture.device(), hot.size() * sizeof(hot[0u]), kGridParticleNumericalStride, hot.data());
            view.m_readonly = makeBuffer(
                fixture.device(), cold.size() * sizeof(cold[0u]), kGridParticleReadonlyStride, cold.data());
            view.m_metadata = metadata;
            view.m_live_epoch = live_epoch;
            view.m_world = grid.activeWorldIdentity();
            view.m_layout = kGridParticleNumericalLayout;
            view.m_stride = kGridParticleNumericalStride;
            view.m_readonly_stride = kGridParticleReadonlyStride;
            view.m_origin = 3.0;
            view.m_device_epoch = epoch;
            view.m_publication = epoch;
            view.m_terrain_revision = 1u;
            view.m_joint_tick = epoch;
            return view;
        };

        const std::array<float, 2u> positions{left_x_m, right_x_m};
        auto live = std::make_shared<std::atomic<u64>>(1u);
        auto hot = makeHot(positions);
        auto cold = makeCold(2u);
        auto metadata = makeMetadata(2u, 1u);
        auto view = makeView(metadata, hot, cold, live, 1u);
        PPR_TEST_ASSERT(view.current());

        GridStagedFrame stage{};
        stage.m_world_edge = WorldLightingPass::kWorldEdge;
        stage.m_cell_size_m = 1.0f;
        stage.m_terrain_visuals = {solid};
        stage.m_tiles.push_back(
            GridTileSubmission{.m_dirty_mask = 1u, .m_tile_range = {0, 0, 128, 128}, .m_chunk_id = 0u});
        stage.m_gpu_particles = view;
        PPR_TEST_ASSERT(not grid.applyStagedFrame(stage));
        PPR_TEST_ASSERT(not grid.applyGpuParticleView(view));

        // A 64-pixel orthographic view. `rhi::getOrthoMatrix` is
        // `orthoD3D(0, width, 0, height, 0, 1)`, i.e. an OFF-CENTRE view box: the
        // visible world rectangle is origin + [0, W] * right + [0, H] * up, with
        // W = H = viewport * ortho_scale, not origin +/- extent/2. Place the origin
        // half an extent back along the screen axes so the requested world point is
        // the window centre, exactly as the density cohort fixture does.
        const auto snapshotFor = [](const float centre_x_m, const float scale) {
            constexpr float viewport_px = 64.0f;
            CameraModel model{};
            model.m_camera_mode = ECameraProjection::orthographic;
            model.m_origin = float3{0.0f, 0.0f, -0.5f};
            model.m_ortho_scale = scale;
            Camera camera{ECameraProjection::orthographic};
            camera.updateModel(TimeSpan{}, model, Viewport{PixelRect{0, 0, 64, 64}});
            const float half_extent = 0.5f * viewport_px * scale;
            const float3 centre{centre_x_m, 1.0f, -0.5f};
            model.m_origin = centre - camera.getRight() * half_extent - camera.getUp() * half_extent;
            camera.updateModel(TimeSpan{}, model, Viewport{PixelRect{0, 0, 64, 64}});
            return camera.getSnapshot();
        };

        rhi::TextureDesc target_description{};
        target_description.type = rhi::TextureType::Texture2D;
        target_description.size = {.width = 64u, .height = 64u, .depth = 1u};
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

        rhi::TextureDesc depth_description = target_description;
        depth_description.format = rhi::Format::D32Float;
        depth_description.usage = rhi::TextureUsage::DepthStencil;
        depth_description.defaultState = rhi::ResourceState::DepthWrite;
        rhi::ComPtr<rhi::ITexture> depth{};
        rhi::ComPtr<rhi::ITextureView> depth_view{};
        PPR_TEST_ASSERT(
            not make_error_code(fixture.device().createTexture(depth_description, nullptr, depth.writeRef())));
        PPR_TEST_ASSERT(not make_error_code(depth->getDefaultView(depth_view.writeRef())));

        const auto viewport = rhi::Viewport::fromSize(64.0f, 64.0f);
        const auto scissor = rhi::ScissorRect::fromSize(64u, 64u);
        const auto prepare = [&](const PreRenderContext &context) -> std::error_code {
            return grid.prepareGpuParticles(context);
        };
        const auto draw = [&](const DrawContext &context) -> std::error_code {
            PPR_RETURN_ERROR_ON_FAIL(GridPass, grid.render(context));
            PPR_RETURN_ERROR_ON_FAIL(GridPass, grid.renderSolidLiquidParticles(context));
            return grid.renderGasParticles(context);
        };
        DrawSubmission submission{"held particle recull", DrawCallback{draw}};
        submission.m_viewport = viewport;
        submission.m_scissor = scissor;
        rhi::ComPtr<rhi::ITextureView> target_view{};
        PPR_TEST_ASSERT(not make_error_code(target->getDefaultView(target_view.writeRef())));
        rhi::RenderPassColorAttachment color{};
        color.view = target_view.get();
        color.clearValue[3] = 1.0f;
        rhi::RenderPassDepthStencilAttachment depth_attachment{};
        depth_attachment.view = depth_view.get();
        depth_attachment.depthClearValue = 1.0f;
        rhi::RenderPassDesc pass_description{};
        pass_description.colorAttachments = &color;
        pass_description.colorAttachmentCount = 1u;
        pass_description.depthStencilAttachment = &depth_attachment;

        const auto encode = [&]() -> std::error_code {
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, renderer.render(
                              string_literal{std::in_place, "held particle recull"}, pass_description, {submission},
                              PreRenderCallback{prepare}));
            grid.notifyFrameSubmitted(renderer.lastSubmittedFenceValue());
            PPR_RETURN_ERROR_ON_FAIL(GridPass, renderer.waitOnHost());
            return renderer.retireCompletedFrames();
        };

        const auto readArgs = [&]() {
            rhi::IndirectDrawArguments arguments{};
            rhi::IBuffer *const buffer = grid.particleBuffersForTest()[1u];
            PPR_TEST_ASSERT(buffer != nullptr);
            PPR_TEST_ASSERT(
                not make_error_code(fixture.device().readBuffer(buffer, 0u, sizeof(arguments), &arguments)));
            return arguments;
        };
        const auto readIds = [&](const u32 count) {
            Array<u32> ids(count, 0u);
            rhi::IBuffer *const buffer = grid.particleBuffersForTest()[2u];
            PPR_TEST_ASSERT(buffer != nullptr);
            if (not ids.empty()) {
                PPR_TEST_ASSERT(
                    not make_error_code(fixture.device().readBuffer(buffer, 0u, ids.size() * sizeof(u32), ids.data())));
            }

            return ids;
        };
        const auto readOrientation = [&](const u32 count) {
            Array<ParticleOrientationGpu> rows(count);
            rhi::IBuffer *const accepted = grid.particleOrientationAcceptedForTest();
            PPR_TEST_ASSERT(accepted != nullptr);
            PPR_TEST_ASSERT(not make_error_code(
                fixture.device().readBuffer(accepted, 0u, rows.size() * sizeof(ParticleOrientationGpu), rows.data())));
            return rows;
        };
        const auto sameOrientation = [](const auto &left, const auto &right) {
            return left.size() == right.size() and
                   (left.empty() or std::memcmp(left.data(), right.data(), left.size() * sizeof(left[0u])) == 0);
        };

        // IEEE binary16 decode for the record probe: exact, including signed zeros
        // and subnormals. `sign * magnitude` with `sign` decoded as +/-0 would lose
        // every subnormal, so the sign is applied to the magnitude explicitly.
        const auto halfValue = [](const u16 bits) {
            constexpr float subnormal_unit = std::bit_cast<float>(0x33800000u); // 2^-24
            const u32 exponent = (bits >> 10u) & 0x1fu;
            const u32 mantissa = bits & 0x3ffu;
            float magnitude{};
            if (exponent == 0u) {
                magnitude = static_cast<float>(mantissa) * subnormal_unit;
            } else if (exponent == 0x1fu) {
                return std::bit_cast<float>(0x7f800000u | (mantissa << 13u)) * ((bits & 0x8000u) != 0u ? -1.0f : 1.0f);
            } else {
                magnitude = std::bit_cast<float>(((exponent + 112u) << 23u) | (mantissa << 13u));
            }

            return (bits & 0x8000u) != 0u ? -magnitude : magnitude;
        };
        // Subnormal and signed-zero literals: the encoder/decoder pair is exact here.
        PPR_TEST_ASSERT(halfValue(0x0001u) == 5.9604644775390625e-08f); // +2^-24
        PPR_TEST_ASSERT(halfValue(0x8001u) == -5.9604644775390625e-08f); // -2^-24
        PPR_TEST_ASSERT(halfValue(0x03ffu) == 6.097555160522461e-05f); // 1023 * 2^-24
        PPR_TEST_ASSERT(halfValue(0x0000u) == 0.0f and not std::signbit(halfValue(0x0000u)));
        PPR_TEST_ASSERT(halfValue(0x8000u) == 0.0f and std::signbit(halfValue(0x8000u)));

        // Camera A: a 4 m window centred on x = 0, so x in [-2, 2], y in [-1, 3].
        // The left row at x = 1 is inside; the right row at x = 9 is 7 m outside.
        PPR_TEST_ASSERT(not grid.update(TimeSpan{}, snapshotFor(0.0f, 0.0625f)));
        PPR_TEST_ASSERT(not encode());
        PPR_TEST_ASSERT(grid.particlePresentationForTest() == EParticlePresentation::current);
        PPR_TEST_ASSERT(grid.particlePackedCountForTest() == 2u);
        const u32 camera_a_visible = readArgs().instanceCount;
        PPR_TEST_ASSERT(camera_a_visible == 1u);
        const auto camera_a_ids = readIds(1u);
        PPR_TEST_ASSERT(camera_a_ids[0u] == 0u);

        const auto packed_epoch = grid.particlePackedEpochForTest();
        const auto packed_encodes = grid.particlePackEncodesForTest();
        const auto retained_buffers = grid.particleBuffersForTest();
        const auto retained_conic = grid.particleConicForTest();
        const auto retained_orientation = readOrientation(2u);
        const auto retained_role = grid.particleOrientationAcceptedRoleForTest();
        // The retained packed rows themselves, byte for byte: a hold must reuse them
        // untouched (no pack, no solver read).
        const auto readPackedRows = [&]() {
            Array<std::byte> bytes(std::size_t{grid.particlePackedCountForTest()} * sizeof(ParticleGpu), std::byte{0});
            rhi::IBuffer *const buffer = grid.particleBuffersForTest()[0u];
            PPR_TEST_ASSERT(buffer != nullptr);
            PPR_TEST_ASSERT(not make_error_code(fixture.device().readBuffer(buffer, 0u, bytes.size(), bytes.data())));
            return bytes;
        };
        const auto retained_rows = readPackedRows();

        // The GPU record matches the canonical word it was derived from. Each row
        // carries a distinct cue (different axis, opposite-signed cross term), so
        // the signed coefficients pin the derivation, the writer's pair layout and
        // the decoder's row parity; the spectrum alone could not tell them apart.
        PPR_TEST_ASSERT(retained_conic != nullptr);
        const u32 conic_words = 3u * ((grid.particlePackedCountForTest() + 1u) / 2u);
        const auto readConic = [&]() {
            Array<u32> words(conic_words, 0u);
            PPR_TEST_ASSERT(not make_error_code(
                fixture.device().readBuffer(retained_conic, 0u, words.size() * sizeof(u32), words.data())));
            return words;
        };
        const Array<u32> conic = readConic();
        float worst_eigenvalue_error{};
        std::array<float, 2u> cross_terms{};
        for (u32 row = 0u; row < grid.particlePackedCountForTest(); ++row) {
            const u32 cue = retained_orientation[row].m_angle_bias_stretch;
            const u32 bias = (cue >> 16u) & 0xffu;
            const u32 stretch = (cue >> 24u) & 0xffu;
            const float anisotropy = kGridParticleTangentContourBias * (static_cast<float>(bias) / 255.0f);
            const float stretch_scale = 1.0f +
                                        (kGridParticleStretchCap - 1.0f) * (static_cast<float>(stretch) / 255.0f);
            const float along = 1.0f / (stretch_scale * stretch_scale);
            const float across_ratio = (1.0f + 0.5f * anisotropy) / (1.0f - 0.5f * anisotropy);
            const float across = across_ratio * across_ratio;
            const float radians = static_cast<float>(cue & 0xffffu) * std::numbers::pi_v<float> / 65536.0f;
            const float cosine = std::cos(radians);
            const float sine = std::sin(radians);
            const float expected_a = cosine * cosine * along + sine * sine * across;
            const float expected_b = cosine * sine * (along - across);
            const float expected_c = sine * sine * along + cosine * cosine * across;

            const std::size_t base = 3u * (row / 2u) + row % 2u;
            u16 a_bits{}, b_bits{}, c_bits{};
            if (row % 2u == 0u) {
                a_bits = static_cast<u16>(conic[base] & 0xffffu);
                b_bits = static_cast<u16>(conic[base] >> 16u);
                c_bits = static_cast<u16>(conic[base + 1u] & 0xffffu);
            } else {
                a_bits = static_cast<u16>(conic[base] >> 16u);
                b_bits = static_cast<u16>(conic[base + 1u] & 0xffffu);
                c_bits = static_cast<u16>(conic[base + 1u] >> 16u);
            }

            const float a = halfValue(a_bits);
            const float b = halfValue(b_bits);
            const float c = halfValue(c_bits);
            // Signed, componentwise: a swapped parity, a flipped cross term or a
            // rotated metric cannot satisfy these against the row's own cue.
            PPR_TEST_ASSERT(std::abs(a - expected_a) <= 1.0f / 1024.0f);
            PPR_TEST_ASSERT(std::abs(b - expected_b) <= 1.0f / 4096.0f);
            PPR_TEST_ASSERT(std::abs(c - expected_c) <= 1.0f / 1024.0f);
            PPR_TEST_ASSERT(expected_b * expected_b > 0.0f);

            const float difference = a - c;
            const float lambda_min = 0.5f * (a + c - std::sqrt(difference * difference + 4.0f * b * b));
            const float lambda_max = a + c - lambda_min;
            // binary16 half-rounding of the two diagonals bounds a metric
            // eigenvalue shift by 2 * 2^-10 + 2^-12, and the determinant follows;
            // these probes pin the derivation and the packing, not bit identity.
            const float tolerance = 4.0f / 1024.0f;
            PPR_TEST_ASSERT(std::abs(lambda_min - along) <= tolerance);
            PPR_TEST_ASSERT(std::abs(lambda_max - across) <= tolerance);
            // sqrt(det) is 1/det(K) = 1/(k eta), and eta = 1/across_ratio.
            PPR_TEST_ASSERT(std::abs(std::sqrt(a * c - b * b) - across_ratio / stretch_scale) <= tolerance);
            // The decoded support can never exceed 3 r: 1.5 R.
            PPR_TEST_ASSERT(1.0f / std::sqrt(lambda_min) <= kGridParticleStretchCap + tolerance);
            cross_terms[row] = b;
            worst_eigenvalue_error = std::max(
                {worst_eigenvalue_error, std::abs(lambda_min - along), std::abs(lambda_max - across)});
        }

        // The two rows carry cues from opposite motion quadrants, so their cross
        // terms have opposite signs while their eigenvalues agree: an implementation
        // that collapsed the cue or swapped the row parity fails here.
        PPR_TEST_ASSERT(cross_terms[0u] < 0.0f and cross_terms[1u] > 0.0f);
        PPR_TEST_ASSERT(cross_terms[0u] * cross_terms[1u] < 0.0f);

        // Every checkpoint reads the compacted IDs once and logs the actual values
        // before asserting, so a future failure reports the observed array rather
        // than a guessed permutation. IDs are global packed-row indices and must be
        // strictly ascending: the design resolves an equal-distance tie to the
        // first (lowest-row) instance, which a reordered compaction would break.
        const auto checkIds = [&](const char *const label, const Array<u32> &ids) {
            const u32 published = readArgs().instanceCount;
            std::string joined{};
            for (const u32 id: ids) {
                joined += std::format("{} ", id);
            }

            _.log(
                std::format(
                    "WORLD_LIGHTING_MATERIAL held_recull {} ids=[{}] args={} slot={} dispatches={}", label, joined,
                    published, grid.particleBuffersForTest() == retained_buffers ? "retained" : "rotated",
                    grid.observations().m_dispatch_commands));
            PPR_TEST_ASSERT(ids.size() == published);
            for (std::size_t index = 0u; index < ids.size(); ++index) {
                PPR_TEST_ASSERT(ids[index] < grid.particlePackedCountForTest());
                if (index != 0u) {
                    PPR_TEST_ASSERT(ids[index - 1u] < ids[index]);
                }
            }
        };

        // A repeated publication with an unchanged camera reuses the retained
        // compaction exactly: same slot buffers, same visible set, no dispatch and
        // the same accepted history. The publisher bumps the shared live epoch in
        // place, exactly as the production hold does, so the retained view is no
        // longer current while its allocation stays valid.
        PPR_TEST_ASSERT(live != nullptr);
        live->store(view.m_device_epoch + 1u, std::memory_order_release);
        PPR_TEST_ASSERT(not view.current());
        PPR_TEST_ASSERT(not grid.applyGpuParticleView(view));
        PPR_TEST_ASSERT(grid.particlePresentationForTest() == EParticlePresentation::held);

        const auto dispatches_before_hold = grid.observations().m_dispatch_commands;
        PPR_TEST_ASSERT(not encode());
        const auto unchanged = grid.observations();
        PPR_TEST_ASSERT(unchanged.m_dispatch_commands == dispatches_before_hold);
        PPR_TEST_ASSERT(grid.particleHoldAttemptsForTest() == 1u);
        PPR_TEST_ASSERT(grid.particleBuffersForTest() == retained_buffers);
        PPR_TEST_ASSERT(grid.particlePackedEpochForTest() == packed_epoch);
        PPR_TEST_ASSERT(grid.particlePackEncodesForTest() == packed_encodes);
        PPR_TEST_ASSERT(grid.particleOrientationAcceptedRoleForTest() == retained_role);
        const auto hold_ids = readIds(1u);
        checkIds("unchanged hold", hold_ids);
        PPR_TEST_ASSERT(hold_ids[0u] == 0u);
        // A reuse re-derives nothing, so the derived record is byte-identical.
        PPR_TEST_ASSERT(readConic() == conic);
        PPR_TEST_ASSERT(readPackedRows() == retained_rows);

        // Camera C: a 16 m window centred on x = 5, so x in [-3, 13], y in [-7, 9].
        // Both retained rows are inside with at least 3 m of margin against the
        // 3.0625 r = 0.153 m cull radius, so the row that camera A culled must
        // appear, and the recull costs the existing three dispatches.
        PPR_TEST_ASSERT(not grid.update(TimeSpan{}, snapshotFor(5.0f, 0.25f)));
        const auto dispatches_before_recull = grid.observations().m_dispatch_commands;
        PPR_TEST_ASSERT(not encode());
        PPR_TEST_ASSERT(grid.particleHoldAttemptsForTest() == 2u);
        PPR_TEST_ASSERT(grid.particlePresentationForTest() == EParticlePresentation::held);
        const u32 camera_c_visible = readArgs().instanceCount;
        PPR_TEST_ASSERT(camera_c_visible == 2u);
        const auto slot_one_ids = readIds(2u);
        checkIds("recull slot 1", slot_one_ids);
        PPR_TEST_ASSERT(slot_one_ids == Array<u32>{0u, 1u});
        PPR_TEST_ASSERT(grid.observations().m_dispatch_commands == dispatches_before_recull + 3u);
        PPR_TEST_ASSERT(grid.particlePackedEpochForTest() == packed_epoch);
        PPR_TEST_ASSERT(grid.particlePackEncodesForTest() == packed_encodes);
        PPR_TEST_ASSERT(grid.particlePackedCountForTest() == 2u);
        PPR_TEST_ASSERT(grid.particleOrientationAcceptedRoleForTest() == retained_role);
        PPR_TEST_ASSERT(sameOrientation(readOrientation(2u), retained_orientation));
        // A recull re-derives the same record from the same accepted word.
        PPR_TEST_ASSERT(readConic() == conic);

        // Camera B: the same 4 m window centred on x = 8, so x in [6, 10]. The
        // right row stays and the left row leaves the frustum, while the retained
        // publication identity is unchanged.
        PPR_TEST_ASSERT(not grid.update(TimeSpan{}, snapshotFor(8.0f, 0.0625f)));
        PPR_TEST_ASSERT(not encode());
        PPR_TEST_ASSERT(grid.particleHoldAttemptsForTest() == 3u);
        const auto slot_two_ids = readIds(1u);
        checkIds("recull slot 2", slot_two_ids);
        PPR_TEST_ASSERT(slot_two_ids[0u] == 1u);
        PPR_TEST_ASSERT(grid.particlePackedEpochForTest() == packed_epoch);
        PPR_TEST_ASSERT(grid.particlePackEncodesForTest() == packed_encodes);
        PPR_TEST_ASSERT(grid.particleOrientationAcceptedRoleForTest() == retained_role);
        PPR_TEST_ASSERT(sameOrientation(readOrientation(2u), retained_orientation));

        // Back to camera C: the third ring slot, so every prepared slot has been
        // written by a held recull root rather than only by the pack frame.
        PPR_TEST_ASSERT(not grid.update(TimeSpan{}, snapshotFor(5.0f, 0.25f)));
        PPR_TEST_ASSERT(not encode());
        PPR_TEST_ASSERT(grid.particleHoldAttemptsForTest() == 4u);
        const auto slot_three_ids = readIds(2u);
        checkIds("recull slot 3", slot_three_ids);
        PPR_TEST_ASSERT(slot_three_ids == slot_one_ids);

        // A definitely-unsubmitted changed-camera attempt must not replace the
        // accepted compaction: prepare at camera B and discard before submission,
        // then hold at the committed camera C with no compaction dispatch at all.
        // Retrying camera B must encode exactly the three dispatches again.
        const auto prepareWithoutSubmit = [&](const CameraSnapshot &camera) {
            PPR_TEST_ASSERT(not grid.update(TimeSpan{}, camera));
            rhi::ComPtr<rhi::ICommandEncoder> encoder{};
            PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(encoder.writeRef())));
            FenceWaitHook wait{[&](const FenceCompletionValue value) { return renderer.waitForFenceValue(value); }};
            PreRenderContext context{fixture.device(), *encoder, renderer.completedFenceValue(), std::move(wait)};
            PPR_TEST_ASSERT(not grid.prepareGpuParticles(context));
            grid.discardUnsubmittedParticles();
        };
        prepareWithoutSubmit(snapshotFor(8.0f, 0.0625f));
        const auto committed_buffers = grid.particleBuffersForTest();
        PPR_TEST_ASSERT(not grid.update(TimeSpan{}, snapshotFor(5.0f, 0.25f)));
        const auto dispatches_after_rejection = grid.observations().m_dispatch_commands;
        PPR_TEST_ASSERT(not encode());
        PPR_TEST_ASSERT(grid.observations().m_dispatch_commands == dispatches_after_rejection);
        PPR_TEST_ASSERT(grid.particleBuffersForTest() == committed_buffers);
        PPR_TEST_ASSERT(readIds(2u) == slot_one_ids);
        PPR_TEST_ASSERT(grid.particlePackedEpochForTest() == packed_epoch);
        PPR_TEST_ASSERT(grid.particlePackEncodesForTest() == packed_encodes);

        PPR_TEST_ASSERT(not grid.update(TimeSpan{}, snapshotFor(8.0f, 0.0625f)));
        const auto dispatches_before_retry = grid.observations().m_dispatch_commands;
        PPR_TEST_ASSERT(not encode());
        PPR_TEST_ASSERT(grid.observations().m_dispatch_commands == dispatches_before_retry + 3u);
        PPR_TEST_ASSERT(readIds(1u)[0u] == 1u);

        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL held_recull rows={} eigenvalues={:.3e} visible=({},{},{}) discharges={} "
                "packs={} epoch={}",
                grid.particlePackedCountForTest(), worst_eigenvalue_error, camera_a_visible, camera_c_visible,
                slot_two_ids.size(), grid.observations().m_dispatch_commands, grid.particlePackEncodesForTest(),
                grid.particlePackedEpochForTest()));
    };

    PPR_UNIT_TEST (held_recull_keeps_global_ascending_ids_across_blocks) {
        // A 4097-row publication whose only visible rows straddle the 256-lane group
        // boundary (rows 255/256) and blocks 15/16 (rows 4095/4096). A held recull
        // must reproduce the normal pack's complete ascending ID list, which is what
        // distinguishes a correct global block prefix from a merely correct
        // single-group result. 4097 is odd, so the final conic pair also covers the
        // neutral unused partner.
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

        GridTerrainVisual solid{};
        solid.m_material_id = 1u;
        solid.m_color_rgba = 0xff8040ffu;
        solid.m_ior = 1.0f;
        solid.m_roughness = 0.4f;
        solid.m_opacity = 1.0f;
        solid.m_surface_flags = kGridSurfaceParticleSolid;

        constexpr u32 row_count = 4097u;
        constexpr u32 visible_row_count = 6u;
        constexpr std::array<u32, visible_row_count> visible_rows{0u, 1u, 255u, 256u, 4095u, 4096u};
        constexpr float particle_radius_m = 0.05f;
        constexpr float origin_cells = 3.0f;

        // The visible rows share one in-window point; every other row sits far
        // outside both cameras, so the visibility mask is identical for the pack
        // and the recull while the camera itself moves.
        const auto makeMetadata = [&](const u64 version) {
            Array<GridParticleMetadataRow> rows(row_count);
            const auto cell = lighting::packCell(1u, 300.0);
            PPR_TEST_ASSERT(cell.has_value());
            for (u32 row = 0u; row < row_count; ++row) {
                rows[row].m_id_low = row + 1u;
                rows[row].m_source_row = row;
                rows[row].m_material_temperature = *cell;
                rows[row].m_radius = particle_radius_m;
            }

            auto metadata = prepareGridParticleMetadata(
                fixture.device(), std::span<const GridParticleMetadataRow>{rows.data(), rows.size()}, row_count, 1.0,
                version);
            PPR_TEST_ASSERT(metadata.has_value());
            return *metadata;
        };
        auto live = std::make_shared<std::atomic<u64>>(1u);
        // The numerical and readonly staging rows must match their declared strides
        // (64 and 24 bytes), so each has its own array type.
        const auto makeHot = [&]() {
            Array<std::array<float, 16u>> rows(row_count);
            for (u32 row = 0u; row < row_count; ++row) {
                const bool visible = std::ranges::find(visible_rows, row) != visible_rows.end();
                rows[row][0u] = (visible ? 1.0f : 2000.0f) + origin_cells;
                rows[row][1u] = (visible ? 1.0f : 2000.0f) + origin_cells;
                rows[row][15u] = 1.0f;
            }

            return rows;
        };
        const auto makeCold = [&]() {
            Array<std::array<float, 6u>> rows(row_count);
            for (auto &row: rows) {
                row[1u] = std::numbers::pi_v<float> * particle_radius_m * particle_radius_m;
            }

            return rows;
        };
        const auto hot = makeHot();
        const auto cold = makeCold();
        auto metadata = makeMetadata(1u);
        GridParticleGpuView view{};
        view.m_numerical = makeBuffer(
            fixture.device(), hot.size() * sizeof(hot[0u]), kGridParticleNumericalStride, hot.data());
        view.m_readonly = makeBuffer(
            fixture.device(), cold.size() * sizeof(cold[0u]), kGridParticleReadonlyStride, cold.data());
        view.m_metadata = metadata;
        view.m_live_epoch = live;
        view.m_world = grid.activeWorldIdentity();
        view.m_layout = kGridParticleNumericalLayout;
        view.m_stride = kGridParticleNumericalStride;
        view.m_readonly_stride = kGridParticleReadonlyStride;
        view.m_origin = 3.0;
        view.m_device_epoch = 1u;
        view.m_publication = 1u;
        view.m_terrain_revision = 1u;
        view.m_joint_tick = 1u;
        PPR_TEST_ASSERT(view.current());

        GridStagedFrame stage{};
        stage.m_world_edge = WorldLightingPass::kWorldEdge;
        stage.m_cell_size_m = 1.0f;
        stage.m_terrain_visuals = {solid};
        stage.m_tiles.push_back(
            GridTileSubmission{.m_dirty_mask = 1u, .m_tile_range = {0, 0, 128, 128}, .m_chunk_id = 0u});
        stage.m_gpu_particles = view;
        PPR_TEST_ASSERT(not grid.applyStagedFrame(stage));
        PPR_TEST_ASSERT(not grid.applyGpuParticleView(view));

        // Same helper as the two-row fixture: the orthographic window is
        // origin + [0, extent], so the origin is placed half an extent back.
        const auto snapshotFor = [](const float centre_x_m, const float scale) {
            constexpr float viewport_px = 64.0f;
            CameraModel model{};
            model.m_camera_mode = ECameraProjection::orthographic;
            model.m_origin = float3{0.0f, 0.0f, -0.5f};
            model.m_ortho_scale = scale;
            Camera camera{ECameraProjection::orthographic};
            camera.updateModel(TimeSpan{}, model, Viewport{PixelRect{0, 0, 64, 64}});
            const float half_extent = 0.5f * viewport_px * scale;
            const float3 centre{centre_x_m, 1.0f, -0.5f};
            model.m_origin = centre - camera.getRight() * half_extent - camera.getUp() * half_extent;
            camera.updateModel(TimeSpan{}, model, Viewport{PixelRect{0, 0, 64, 64}});
            return camera.getSnapshot();
        };

        rhi::TextureDesc target_description{};
        target_description.type = rhi::TextureType::Texture2D;
        target_description.size = {.width = 64u, .height = 64u, .depth = 1u};
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
        rhi::TextureDesc depth_description = target_description;
        depth_description.format = rhi::Format::D32Float;
        depth_description.usage = rhi::TextureUsage::DepthStencil;
        depth_description.defaultState = rhi::ResourceState::DepthWrite;
        rhi::ComPtr<rhi::ITexture> depth{};
        rhi::ComPtr<rhi::ITextureView> depth_view{};
        PPR_TEST_ASSERT(
            not make_error_code(fixture.device().createTexture(depth_description, nullptr, depth.writeRef())));
        PPR_TEST_ASSERT(not make_error_code(depth->getDefaultView(depth_view.writeRef())));

        const auto viewport = rhi::Viewport::fromSize(64.0f, 64.0f);
        const auto scissor = rhi::ScissorRect::fromSize(64u, 64u);
        const auto prepare = [&](const PreRenderContext &context) -> std::error_code {
            return grid.prepareGpuParticles(context);
        };
        const auto draw = [&](const DrawContext &context) -> std::error_code {
            PPR_RETURN_ERROR_ON_FAIL(GridPass, grid.render(context));
            PPR_RETURN_ERROR_ON_FAIL(GridPass, grid.renderSolidLiquidParticles(context));
            return grid.renderGasParticles(context);
        };
        DrawSubmission submission{"held recull cross-block order", DrawCallback{draw}};
        submission.m_viewport = viewport;
        submission.m_scissor = scissor;
        rhi::ComPtr<rhi::ITextureView> target_view{};
        PPR_TEST_ASSERT(not make_error_code(target->getDefaultView(target_view.writeRef())));
        rhi::RenderPassColorAttachment color{};
        color.view = target_view.get();
        color.clearValue[3] = 1.0f;
        rhi::RenderPassDepthStencilAttachment depth_attachment{};
        depth_attachment.view = depth_view.get();
        depth_attachment.depthClearValue = 1.0f;
        rhi::RenderPassDesc pass_description{};
        pass_description.colorAttachments = &color;
        pass_description.colorAttachmentCount = 1u;
        pass_description.depthStencilAttachment = &depth_attachment;

        const auto encode = [&]() -> std::error_code {
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, renderer.render(
                              string_literal{std::in_place, "held recull cross-block order"}, pass_description,
                              {submission}, PreRenderCallback{prepare}));
            grid.notifyFrameSubmitted(renderer.lastSubmittedFenceValue());
            PPR_RETURN_ERROR_ON_FAIL(GridPass, renderer.waitOnHost());
            return renderer.retireCompletedFrames();
        };
        const auto readIds = [&]() {
            Array<u32> ids(visible_row_count, 0u);
            rhi::IBuffer *const buffer = grid.particleBuffersForTest()[2u];
            PPR_TEST_ASSERT(buffer != nullptr);
            PPR_TEST_ASSERT(
                not make_error_code(fixture.device().readBuffer(buffer, 0u, ids.size() * sizeof(u32), ids.data())));
            return ids;
        };
        const auto checkIds = [&](const char *const label, const Array<u32> &ids) {
            const rhi::IndirectDrawArguments arguments = [&] {
                rhi::IndirectDrawArguments value{};
                rhi::IBuffer *const buffer = grid.particleBuffersForTest()[1u];
                PPR_TEST_ASSERT(buffer != nullptr);
                PPR_TEST_ASSERT(not make_error_code(fixture.device().readBuffer(buffer, 0u, sizeof(value), &value)));
                return value;
            }();
            std::string joined{};
            for (const u32 id: ids) {
                joined += std::format("{} ", id);
            }

            _.log(
                std::format(
                    "WORLD_LIGHTING_MATERIAL held_recull {} ids=[{}] args={} dispatches={}", label, joined,
                    arguments.instanceCount, grid.observations().m_dispatch_commands));
            PPR_TEST_ASSERT(arguments.instanceCount == visible_row_count);
            for (std::size_t index = 0u; index < ids.size(); ++index) {
                PPR_TEST_ASSERT(ids[index] == visible_rows[index]);
            }
        };

        // Camera A: a 4 m window at (0, 1). The six selected rows are at (1, 1); the
        // other 4091 rows are at (2000, 2000) and cannot enter either window.
        PPR_TEST_ASSERT(not grid.update(TimeSpan{}, snapshotFor(0.0f, 0.0625f)));
        PPR_TEST_ASSERT(not encode());
        PPR_TEST_ASSERT(grid.particlePresentationForTest() == EParticlePresentation::current);
        PPR_TEST_ASSERT(grid.particlePackedCountForTest() == row_count);
        const auto packed_ids = readIds();
        checkIds("normal pack", packed_ids);

        const auto packed_encodes = grid.particlePackEncodesForTest();
        const auto packed_epoch = grid.particlePackedEpochForTest();

        // The final conic pair: row 4096 is even and its odd partner 4097 is past the
        // count, so the pair is (neutral row, neutral partner). Exact words pin the
        // writer's pair layout and the odd tail on the GPU.
        rhi::IBuffer *const conic = grid.particleConicForTest();
        PPR_TEST_ASSERT(conic != nullptr);
        PPR_TEST_ASSERT(conic->getDesc().size == gridParticleConicBytes(row_count));
        const std::size_t last_pair_word = 3u * ((row_count + 1u) / 2u) - 3u;
        std::array<u32, 3u> tail{};
        PPR_TEST_ASSERT(not make_error_code(
            fixture.device().readBuffer(conic, last_pair_word * sizeof(u32), sizeof(tail), tail.data())));
        PPR_TEST_ASSERT(tail[0u] == 0x0000'3c00u); // a(4096) = 1, b(4096) = 0
        PPR_TEST_ASSERT(tail[1u] == 0x3c00'3c00u); // c(4096) = 1, a(4097 partner) = 1
        PPR_TEST_ASSERT(tail[2u] == 0x3c00'0000u); // b(4097 partner) = 0, c(4097 partner) = 1

        // Hold at a slightly wider camera with the same visibility mask: the recull
        // must reproduce the pack's full ascending list exactly.
        live->store(view.m_device_epoch + 1u, std::memory_order_release);
        PPR_TEST_ASSERT(not view.current());
        PPR_TEST_ASSERT(not grid.applyGpuParticleView(view));
        PPR_TEST_ASSERT(not grid.update(TimeSpan{}, snapshotFor(0.0f, 0.078125f)));
        const auto dispatches_before_recull = grid.observations().m_dispatch_commands;
        PPR_TEST_ASSERT(not encode());
        PPR_TEST_ASSERT(grid.observations().m_dispatch_commands == dispatches_before_recull + 3u);
        PPR_TEST_ASSERT(grid.particlePackEncodesForTest() == packed_encodes);
        PPR_TEST_ASSERT(grid.particlePackedEpochForTest() == packed_epoch);
        const auto recull_ids = readIds();
        checkIds("held recull", recull_ids);
        PPR_TEST_ASSERT(recull_ids == packed_ids);
    };

    PPR_UNIT_TEST (held_publication_keeps_the_accepted_liquid_lane_after_a_rejected_inventory) {
        // The peer review's retained-source rule. A hold draws the accepted packed
        // rows, so admission must be derived from that publication's metadata, not
        // from an incoming view that was installed and then definitely rejected.
        // Accepted liquid A -> rejected same-capacity solid inventory B -> A's epoch
        // revocation -> hold of A must still admit A's liquid lane on A's rows.
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

        GridTerrainVisual liquid{};
        liquid.m_material_id = 1u;
        liquid.m_color_rgba = 0x0000ffffu;
        liquid.m_ior = 1.0f;
        liquid.m_opacity = 1.0f;
        liquid.m_surface_flags = kGridSurfaceParticleLiquid;
        GridTerrainVisual solid{};
        solid.m_material_id = 2u;
        solid.m_color_rgba = 0xff8040ffu;
        solid.m_ior = 1.0f;
        solid.m_roughness = 0.4f;
        solid.m_opacity = 1.0f;
        solid.m_surface_flags = kGridSurfaceParticleSolid;

        constexpr float particle_radius_m = 0.05f;
        constexpr float origin_cells = 3.0f;
        // A's rows are in the camera window; B's are far away, so drawing B's rows
        // instead of A's would present no particles at all.
        const auto makeInventory = [&](const u32 material, const u64 version) {
            Array<GridParticleMetadataRow> rows(2u);
            for (u32 row = 0u; row < 2u; ++row) {
                const auto cell = lighting::packCell(static_cast<u16>(material), 300.0);
                PPR_TEST_ASSERT(cell.has_value());
                rows[row].m_id_low = static_cast<u32>(version) * 10u + row + 1u;
                rows[row].m_source_row = row;
                rows[row].m_material_temperature = *cell;
                rows[row].m_radius = particle_radius_m;
            }

            auto metadata = prepareGridParticleMetadata(
                fixture.device(), std::span<const GridParticleMetadataRow>{rows.data(), rows.size()}, 2u, 1.0, version);
            PPR_TEST_ASSERT(metadata.has_value());
            return *metadata;
        };
        const auto makeView = [&](const std::shared_ptr<const GridParticleMetadataResource> &metadata,
                                  const std::shared_ptr<std::atomic<u64>> &live_epoch, const u64 epoch) {
            Array<std::array<float, 16u>> hot(2u);
            Array<std::array<float, 6u>> cold(2u);
            for (u32 row = 0u; row < 2u; ++row) {
                const bool accepted_inventory = metadata->m_materials.front() == 1u;
                hot[row][0u] = (accepted_inventory ? 1.0f + static_cast<float>(row) : 2000.0f) + origin_cells;
                hot[row][1u] = (accepted_inventory ? 1.0f : 2000.0f) + origin_cells;
                hot[row][15u] = 1.0f;
                cold[row][1u] = std::numbers::pi_v<float> * particle_radius_m * particle_radius_m;
            }

            GridParticleGpuView view{};
            view.m_numerical = makeBuffer(
                fixture.device(), hot.size() * sizeof(hot[0u]), kGridParticleNumericalStride, hot.data());
            view.m_readonly = makeBuffer(
                fixture.device(), cold.size() * sizeof(cold[0u]), kGridParticleReadonlyStride, cold.data());
            view.m_metadata = metadata;
            view.m_live_epoch = live_epoch;
            view.m_world = grid.activeWorldIdentity();
            view.m_layout = kGridParticleNumericalLayout;
            view.m_stride = kGridParticleNumericalStride;
            view.m_readonly_stride = kGridParticleReadonlyStride;
            view.m_origin = 3.0;
            view.m_device_epoch = epoch;
            view.m_publication = epoch;
            view.m_terrain_revision = 1u;
            view.m_joint_tick = epoch;
            return view;
        };

        auto live_a = std::make_shared<std::atomic<u64>>(1u);
        auto live_b = std::make_shared<std::atomic<u64>>(4u);
        auto metadata_a = makeInventory(1u, 1u);
        auto metadata_b = makeInventory(2u, 4u);
        auto view_a = makeView(metadata_a, live_a, 1u);
        auto view_b = makeView(metadata_b, live_b, 4u);
        PPR_TEST_ASSERT(view_a.current() and view_b.current());

        GridStagedFrame stage{};
        stage.m_world_edge = WorldLightingPass::kWorldEdge;
        stage.m_cell_size_m = 1.0f;
        stage.m_terrain_visuals = {liquid, solid};
        stage.m_tiles.push_back(
            GridTileSubmission{.m_dirty_mask = 1u, .m_tile_range = {0, 0, 128, 128}, .m_chunk_id = 0u});
        stage.m_gpu_particles = view_a;
        PPR_TEST_ASSERT(not grid.applyStagedFrame(stage));
        PPR_TEST_ASSERT(not grid.applyGpuParticleView(view_a));

        const auto snapshot = [](const float centre_x_m, const float scale) {
            constexpr float viewport_px = 64.0f;
            CameraModel model{};
            model.m_camera_mode = ECameraProjection::orthographic;
            model.m_origin = float3{0.0f, 0.0f, -0.5f};
            model.m_ortho_scale = scale;
            Camera camera{ECameraProjection::orthographic};
            camera.updateModel(TimeSpan{}, model, Viewport{PixelRect{0, 0, 64, 64}});
            const float half_extent = 0.5f * viewport_px * scale;
            const float3 centre{centre_x_m, 1.0f, -0.5f};
            model.m_origin = centre - camera.getRight() * half_extent - camera.getUp() * half_extent;
            camera.updateModel(TimeSpan{}, model, Viewport{PixelRect{0, 0, 64, 64}});
            return camera.getSnapshot();
        };
        // A 4 m window centred on (1, 1): A's rows at x = 1 and x = 2 are inside;
        // B's rows at x = 2000 are far outside.
        const auto camera = snapshot(1.0f, 0.0625f);

        rhi::TextureDesc target_description{};
        target_description.type = rhi::TextureType::Texture2D;
        target_description.size = {.width = 64u, .height = 64u, .depth = 1u};
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
        rhi::TextureDesc depth_description = target_description;
        depth_description.format = rhi::Format::D32Float;
        depth_description.usage = rhi::TextureUsage::DepthStencil;
        depth_description.defaultState = rhi::ResourceState::DepthWrite;
        rhi::ComPtr<rhi::ITexture> depth{};
        rhi::ComPtr<rhi::ITextureView> depth_view{};
        PPR_TEST_ASSERT(
            not make_error_code(fixture.device().createTexture(depth_description, nullptr, depth.writeRef())));
        PPR_TEST_ASSERT(not make_error_code(depth->getDefaultView(depth_view.writeRef())));

        const auto viewport = rhi::Viewport::fromSize(64.0f, 64.0f);
        const auto scissor = rhi::ScissorRect::fromSize(64u, 64u);
        const auto prepare = [&](const PreRenderContext &context) -> std::error_code {
            return grid.prepareGpuParticles(context);
        };
        const auto draw = [&](const DrawContext &context) -> std::error_code {
            PPR_RETURN_ERROR_ON_FAIL(GridPass, grid.render(context));
            PPR_RETURN_ERROR_ON_FAIL(GridPass, grid.renderSolidLiquidParticles(context));
            return grid.renderGasParticles(context);
        };
        DrawSubmission submission{"held accepted lane admission", DrawCallback{draw}};
        submission.m_viewport = viewport;
        submission.m_scissor = scissor;
        rhi::ComPtr<rhi::ITextureView> target_view{};
        PPR_TEST_ASSERT(not make_error_code(target->getDefaultView(target_view.writeRef())));
        rhi::RenderPassColorAttachment color{};
        color.view = target_view.get();
        color.clearValue[3] = 1.0f;
        rhi::RenderPassDepthStencilAttachment depth_attachment{};
        depth_attachment.view = depth_view.get();
        depth_attachment.depthClearValue = 1.0f;
        rhi::RenderPassDesc pass_description{};
        pass_description.colorAttachments = &color;
        pass_description.colorAttachmentCount = 1u;
        pass_description.depthStencilAttachment = &depth_attachment;

        const auto encode = [&]() -> std::error_code {
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, renderer.render(
                              string_literal{std::in_place, "held accepted lane admission"}, pass_description,
                              {submission}, PreRenderCallback{prepare}));
            grid.notifyFrameSubmitted(renderer.lastSubmittedFenceValue());
            PPR_RETURN_ERROR_ON_FAIL(GridPass, renderer.waitOnHost());
            return renderer.retireCompletedFrames();
        };
        const auto readIds = [&](const u32 count) {
            Array<u32> ids(count, 0u);
            rhi::IBuffer *const buffer = grid.particleBuffersForTest()[2u];
            PPR_TEST_ASSERT(buffer != nullptr);
            PPR_TEST_ASSERT(
                not make_error_code(fixture.device().readBuffer(buffer, 0u, ids.size() * sizeof(u32), ids.data())));
            return ids;
        };

        const auto readArgs = [&]() {
            rhi::IndirectDrawArguments arguments{};
            rhi::IBuffer *const buffer = grid.particleBuffersForTest()[1u];
            PPR_TEST_ASSERT(buffer != nullptr);
            PPR_TEST_ASSERT(
                not make_error_code(fixture.device().readBuffer(buffer, 0u, sizeof(arguments), &arguments)));
            return arguments;
        };

        // Accepted publication A: one admitted liquid lane, both rows visible.
        PPR_TEST_ASSERT(not grid.update(TimeSpan{}, camera));
        PPR_TEST_ASSERT(not encode());
        PPR_TEST_ASSERT(grid.particlePresentationForTest() == EParticlePresentation::current);
        PPR_TEST_ASSERT(grid.particleSurfaceAdmitted());
        PPR_TEST_ASSERT(grid.particleSurfaceClassMask() == (1u << static_cast<u32>(EGridParticleDrawClass::opaque)));
        const auto accepted_ids = readIds(2u);
        PPR_TEST_ASSERT(readArgs().instanceCount == 2u);
        PPR_TEST_ASSERT(accepted_ids == Array<u32>{0u, 1u});
        const auto packed_epoch = grid.particlePackedEpochForTest();
        const auto packed_encodes = grid.particlePackEncodesForTest();

        // Revoke A, install B (current, solid inventory), prepare it and reject the
        // frame before submission. The accepted rows, count and metadata stay A's.
        live_a->store(view_a.m_device_epoch + 1u, std::memory_order_release);
        PPR_TEST_ASSERT(not view_a.current());
        PPR_TEST_ASSERT(not grid.applyGpuParticleView(view_b));
        PPR_TEST_ASSERT(not grid.update(TimeSpan{}, camera));
        {
            rhi::ComPtr<rhi::ICommandEncoder> encoder{};
            PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(encoder.writeRef())));
            FenceWaitHook wait{[&](const FenceCompletionValue value) { return renderer.waitForFenceValue(value); }};
            PreRenderContext context{fixture.device(), *encoder, renderer.completedFenceValue(), std::move(wait)};
            PPR_TEST_ASSERT(not grid.prepareGpuParticles(context));
            grid.discardUnsubmittedParticles();
        }

        // B is a current publication, so that rejected attempt legitimately encoded
        // one pack (its commands never executed and its tokens were dropped). The
        // invariant under test is that the *hold* adds no pack and keeps A's accepted
        // tokens, so the encode baseline is taken after the rejection, and the
        // per-frame sequence is asserted explicitly.
        const auto encodes_after_rejection = grid.particlePackEncodesForTest();
        PPR_TEST_ASSERT(encodes_after_rejection == packed_encodes + 1u);
        PPR_TEST_ASSERT(grid.particlePackedEpochForTest() == packed_epoch);
        PPR_TEST_ASSERT(grid.particlePackedCountForTest() == 2u);

        // Revoke B so the next frame is a hold of A. Admission must be derived from
        // A's accepted metadata; deriving it from the rejected incoming inventory
        // would drop the liquid lane and hand A's rows to the direct classes.
        live_b->store(view_b.m_device_epoch + 1u, std::memory_order_release);
        PPR_TEST_ASSERT(not view_b.current());
        PPR_TEST_ASSERT(not grid.applyGpuParticleView(view_b));
        PPR_TEST_ASSERT(grid.particlePresentationForTest() == EParticlePresentation::held);
        const auto dispatches_before_hold = grid.observations().m_dispatch_commands;
        PPR_TEST_ASSERT(not encode());
        PPR_TEST_ASSERT(grid.observations().m_dispatch_commands == dispatches_before_hold);
        PPR_TEST_ASSERT(grid.particlePresentationForTest() == EParticlePresentation::held);
        PPR_TEST_ASSERT(grid.particleSurfaceAdmitted());
        PPR_TEST_ASSERT(grid.particleSurfaceClassMask() == (1u << static_cast<u32>(EGridParticleDrawClass::opaque)));
        // The admitted phase mask and the class come from the retained accepted
        // owner, so a rejected incoming solid inventory cannot re-route A's rows.
        PPR_TEST_ASSERT(grid.particleSurfaceTargetsForTest().m_phase_mask == 1u);
        const rhi::IndirectDrawArguments held_args = readArgs();
        PPR_TEST_ASSERT(held_args.instanceCount == 2u);
        PPR_TEST_ASSERT(readIds(2u) == accepted_ids);
        PPR_TEST_ASSERT(grid.particlePackedEpochForTest() == packed_epoch);
        PPR_TEST_ASSERT(grid.particlePackEncodesForTest() == encodes_after_rejection);
        PPR_TEST_ASSERT(grid.particlePackedCountForTest() == 2u);

        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL held_lane_admission ids=({},{}) args={} dispatches={} packs={}",
                accepted_ids[0u], accepted_ids[1u], held_args.instanceCount, grid.observations().m_dispatch_commands,
                grid.particlePackEncodesForTest()));
    };

    PPR_UNIT_TEST (opaque_terrain_ownership_occludes_solid_and_liquid_particles) {
        Fixture fixture{};
        fixture.initialize();
        PPR_DEFER {
            fixture.shutdown();
        };

        // Palette slot n carries material id n, except slot 3, which deliberately
        // carries a different id to exercise the palette identity guard the
        // fragment applies before retiring a particle.
        std::array<GridTerrainVisual, 4u> palette{};
        palette[1u].m_material_id = 1u;
        palette[1u].m_color_rgba = 0xffff'ffffu;
        palette[1u].m_surface_flags = 0u;
        palette[2u].m_material_id = 2u;
        palette[2u].m_color_rgba = 0x4080'ffffu;
        palette[2u].m_surface_flags = kGridSurfaceOpticalBlend;
        palette[3u].m_material_id = 4u;
        palette[3u].m_color_rgba = 0xffff'ffffu;

        const auto word = [](const u32 material, const double temperature) {
            const auto packed = lighting::packCell(static_cast<u16>(material), temperature);
            PPR_TEST_ASSERT(packed.has_value());
            return *packed;
        };

        // Destination cells, one per case.
        //   0 opaque terrain-owned, carrying the particle's own material id;
        //   1 the same resolved material id, but a committed particle owns it;
        //   2 a particle-owned dig cell;
        //   3 an empty cell;
        //   4 optical (glass/water) terrain-owned;
        //   5 a material absent from the palette (identity mismatch).
        const std::array<u32, 6u> resolved{
            word(1u, 300.0), word(1u, 500.0), word(1u, 500.0), 0u, word(2u, 300.0), word(3u, 300.0)};
        const std::array<u32, 6u> best_row{0xffff'ffffu, 7u, 7u, 0xffff'ffffu, 0xffff'ffffu, 0xffff'ffffu};

        std::vector<float4> fields(6u * 8u);
        for (u32 index = 0u; index < resolved.size(); ++index) {
            fields[index * 8u + 0u] = float4{static_cast<float>(index), 0.0f, 0.0f, 0.0f};
        }

        auto &device = fixture.device();
        auto palette_buffer = makeBuffer(
            device, palette.size() * sizeof(GridTerrainVisual), sizeof(GridTerrainVisual), palette.data());
        auto resolved_buffer = makeBuffer(device, resolved.size() * sizeof(u32), sizeof(u32), resolved.data());
        auto best_row_buffer = makeBuffer(device, best_row.size() * sizeof(u32), sizeof(u32), best_row.data());
        auto fields_buffer = makeBuffer(device, fields.size() * sizeof(float4), sizeof(float4), fields.data());
        auto detail = makeBuffer(device, resolved.size() * sizeof(float4), sizeof(float4), nullptr, true);

        auto program = fixture.makeProgram(
            std::filesystem::current_path() / "shaders" / "tests" / "world_lighting_material_probe.slang",
            "probeTerrainOcclusion");
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
        frame.m_count = static_cast<u32>(resolved.size());
        rhi::ShaderCursor cursor{root.get()};
        rhi::ShaderCursor constants{};
        PPR_TEST_ASSERT(not make_error_code(cursor["g_material_probe"].getDereferenced(constants)));
        PPR_TEST_ASSERT(not make_error_code(constants.setData(&frame, sizeof(frame))));
        PPR_TEST_ASSERT(not make_error_code(cursor["g_probe_fields"].setBinding(rhi::Binding(fields_buffer))));
        PPR_TEST_ASSERT(not make_error_code(cursor["g_probe_detail"].setBinding(rhi::Binding(detail))));
        PPR_TEST_ASSERT(not make_error_code(cursor["g_resolved_cells"].setBinding(rhi::Binding(resolved_buffer))));
        PPR_TEST_ASSERT(not make_error_code(cursor["g_best_row"].setBinding(rhi::Binding(best_row_buffer))));
        PPR_TEST_ASSERT(not make_error_code(cursor["g_terrain_visuals"].setBinding(rhi::Binding(palette_buffer))));

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
            pass->dispatchCompute(1u, 1u, 1u);
        }
        encoder->setBufferState(detail, rhi::ResourceState::CopySource);
        fixture.submit(*encoder);

        std::vector<float4> result(6u);
        PPR_TEST_ASSERT(
            not make_error_code(device.readBuffer(detail, 0u, sizeof(float4) * result.size(), result.data())));
        const auto occludes = [](const float4 &record) { return record.x != 0.0f; };
        const auto material = [](const float4 &record) { return static_cast<u32>(record.y); };
        const auto particle_owned = [](const float4 &record) { return record.z != 0.0f; };

        for (u32 index = 0u; index < result.size(); ++index) {
            _.log(
                std::format(
                    "WORLD_LIGHTING_MATERIAL particle_occlusion cell={} material={} particle_owned={} occludes={}",
                    index, material(result[index]), particle_owned(result[index]), occludes(result[index])));
        }

        // Opaque terrain-owned cell, carrying the particle's own material id:
        // the committed ownership grid still hides the particle.
        PPR_TEST_ASSERT(material(result[0u]) == 1u);
        PPR_TEST_ASSERT(not particle_owned(result[0u]));
        PPR_TEST_ASSERT(occludes(result[0u]));

        // The same resolved material id with a committed particle winner is not
        // terrain-owned: ownership, not material identity, selects the cell.
        PPR_TEST_ASSERT(material(result[1u]) == 1u);
        PPR_TEST_ASSERT(particle_owned(result[1u]));
        PPR_TEST_ASSERT(not occludes(result[1u]));

        // Particle-owned dig cell: the particle draws.
        PPR_TEST_ASSERT(not occludes(result[2u]));

        // Empty cell: no material, no occlusion.
        PPR_TEST_ASSERT(material(result[3u]) == 0u);
        PPR_TEST_ASSERT(not occludes(result[3u]));

        // Optical (glass/water) terrain-owned: the particle keeps blending under.
        PPR_TEST_ASSERT(material(result[4u]) == 2u);
        PPR_TEST_ASSERT(not particle_owned(result[4u]));
        PPR_TEST_ASSERT(not occludes(result[4u]));

        // Palette identity mismatch: not an admitted terrain material.
        PPR_TEST_ASSERT(material(result[5u]) == 3u);
        PPR_TEST_ASSERT(not occludes(result[5u]));
    };

    PPR_UNIT_TEST (excavated_vacuum_survives_and_opaque_terrain_clips_the_particle) {
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

        GridTerrainVisual particle_material{};
        particle_material.m_material_id = 1u;
        particle_material.m_color_rgba = 0xff8040ffu;
        particle_material.m_ior = 1.0f;
        particle_material.m_roughness = 0.4f;
        particle_material.m_opacity = 1.0f;
        particle_material.m_surface_flags = kGridSurfaceParticleSolid;
        // The solid description composites CH irradiance, the BRDF term and the
        // material's own authored emission. This world publishes an explicit zero
        // fine-CH field, so the authored emission is what makes the painted pixel
        // a deterministic non-clear colour.
        particle_material.m_emissive_rgb[0] = 1.0f;
        particle_material.m_emissive_rgb[1] = 0.5f;
        particle_material.m_emissive_rgb[2] = 0.2f;
        particle_material.m_emissive_strength = 4.0f;

        // Opaque terrain, not on the optical-blend path: drawn by the
        // depth-writing terrain pass and never by the translucent one.
        GridTerrainVisual terrain{};
        terrain.m_material_id = 2u;
        terrain.m_color_rgba = 0x00ff00ffu;
        terrain.m_ior = 1.0f;
        terrain.m_opacity = 1.0f;
        terrain.m_roundness = 0.0f;

        // A translucent terrain material makes the second terrain pass encode a
        // real draw (it paints no cell here), so both terrain passes run.
        GridTerrainVisual translucent{};
        translucent.m_material_id = 3u;
        translucent.m_color_rgba = 0x4080ffffu;
        translucent.m_opacity = 0.5f;
        translucent.m_surface_flags = kGridSurfaceOpticalBlend;

        // 1/16 m per pixel: the 64 px target spans 4 m from the camera origin,
        // so both probe stations are on screen and a 0.25 m drawn support covers
        // a handful of pixels.
        CameraModel model{};
        model.m_camera_mode = ECameraProjection::orthographic;
        model.m_origin = float3{-0.5f, 0.0f, -0.5f};
        model.m_ortho_scale = 1.0f / 16.0f;
        Camera camera{ECameraProjection::orthographic};
        camera.updateModel(TimeSpan{}, model, Viewport{PixelRect{0, 0, 64, 64}});
        const auto snapshot = camera.getSnapshot();

        rhi::TextureDesc target_description{};
        target_description.type = rhi::TextureType::Texture2D;
        target_description.size = {.width = 64u, .height = 64u, .depth = 1u};
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
        const auto viewport = rhi::Viewport::fromSize(64.0f, 64.0f);
        const auto scissor = rhi::ScissorRect::fromSize(64u, 64u);

        // One terrain stage for every capture: cell (1, 2) is an opaque terrain
        // cell; cell (2, 1) stays excavated vacuum.
        GridPass::GridStagedUpload upload{};
        upload.m_payload.m_rect[2] = upload.m_payload.m_rect[3] = 128.0f;
        upload.m_cell_materials.assign(GridPass::kCellsPerChunk, 0u);
        upload.m_cell_materials[2u * 128u + 1u] = 2u;
        GridStagedFrame stage{};
        stage.m_world_edge = WorldLightingPass::kWorldEdge;
        stage.m_cell_size_m = 1.0f;
        stage.m_terrain_visuals = {particle_material, terrain, translucent};
        stage.m_tiles.push_back(
            GridTileSubmission{.m_dirty_mask = 1u, .m_tile_range = {0, 0, 128, 128}, .m_chunk_id = 0u});
        stage.m_uploads.push_back(std::move(upload));
        PPR_TEST_ASSERT(not grid.applyStagedFrame(stage));

        // The terrain passes shade from the published resolved cells, so the
        // same opaque cell is published there (the particle hard clip reads the
        // cell atlas above). Every other cell is vacuum and the fine CH field is
        // an explicit zero, which is what the particle's solid description
        // decodes here.
        Array<u32> resolved(static_cast<std::size_t>(WorldLightingPass::kCellCount), 0u);
        const auto terrain_cell = lighting::packCell(2u, 300.0);
        PPR_TEST_ASSERT(terrain_cell.has_value());
        resolved[2u * static_cast<std::size_t>(WorldLightingPass::kWorldEdge) + 1u] = *terrain_cell;
        auto resolved_buffer = makeBuffer(
            fixture.device(), resolved.size() * sizeof(u32), sizeof(u32), resolved.data());
        Array<lighting::PackedCH> fine_ch(lighting::kFineCHRecordCount, lighting::PackedCH{});
        auto fine_ch_buffer = makeBuffer(
            fixture.device(), fine_ch.size() * sizeof(lighting::PackedCH), sizeof(lighting::PackedCH), fine_ch.data());
        PPR_TEST_ASSERT(not grid.setLightingBuffersForTest(std::move(resolved_buffer), std::move(fine_ch_buffer)));

        const auto live = std::make_shared<std::atomic<u64>>(1u);
        u64 publication = 0u;
        // The particle positions are the only variable: (2.5, 1.5) is an
        // excavated vacuum cell, (1.5, 2.5) sits under the opaque cell above.
        const auto publish = [&](const std::optional<float2> position) {
            GridParticleGpuView view{};
            std::array<std::array<float, 16u>, 1u> hot{};
            std::array<std::array<float, 6u>, 1u> cold{};
            if (position) {
                // Hot rows are native cells and the packer subtracts the view's
                // 3-cell origin, so the authored world metre position lands at
                // `position + 3` with a 1 m cell.
                hot[0u][0u] = position->x + 3.0f;
                hot[0u][1u] = position->y + 3.0f;
                hot[0u][15u] = 1.0f;
                // r = 0.2 m, so the drawn support (R = 2.0 r = 0.4 m) still fits
                // inside one 1 m native cell: the per-pixel terrain hard clip
                // then covers the whole footprint of the occupied probe.
                cold[0u][1u] = std::numbers::pi_v<float> * 0.04f;
            }

            Array<GridParticleMetadataRow> rows{};
            GridParticleMetadataRow row{};
            if (position) {
                const auto cell = lighting::packCell(1u, 300.0);
                PPR_TEST_ASSERT(cell.has_value());
                row.m_id_low = 1u;
                row.m_source_row = 0u;
                row.m_material_temperature = *cell;
                row.m_radius = 0.5f;
                rows.push_back(row);
            }

            const std::span<const GridParticleMetadataRow> published{
                position ? rows.data() : nullptr, position ? rows.size() : 0u};
            const auto metadata = prepareGridParticleMetadata(
                fixture.device(), published, position ? 1u : 0u, 1.0, ++publication);
            PPR_TEST_ASSERT(metadata.has_value());
            view.m_numerical = makeBuffer(fixture.device(), sizeof(hot), kGridParticleNumericalStride, hot.data());
            view.m_readonly = makeBuffer(fixture.device(), sizeof(cold), kGridParticleReadonlyStride, cold.data());
            view.m_metadata = *metadata;
            view.m_live_epoch = live;
            view.m_world = grid.activeWorldIdentity();
            view.m_layout = kGridParticleNumericalLayout;
            view.m_stride = kGridParticleNumericalStride;
            view.m_readonly_stride = kGridParticleReadonlyStride;
            view.m_origin = 3.0;
            view.m_device_epoch = publication;
            view.m_publication = publication;
            view.m_terrain_revision = 1u;
            view.m_joint_tick = publication;
            live->store(publication, std::memory_order_release);
            PPR_TEST_ASSERT(not grid.applyGpuParticleView(view));
        };

        const auto capture = [&]() {
            PPR_TEST_ASSERT(not grid.update(TimeSpan{}, snapshot));
            for (;;) {
                PPR_TEST_ASSERT(not renderer.retireCompletedFrames());
                const auto prepared = grid.tryPrepareCellUploads(fixture.device(), renderer.completedFenceValue());
                PPR_TEST_ASSERT(prepared.has_value());
                if (*prepared) {
                    break;
                }

                PPR_TEST_ASSERT(not renderer.waitOnHost());
            }

            const auto prepare = [&](const PreRenderContext &context) -> std::error_code {
                // Same order as the Editor: the terrain visibility table first,
                // then the particle pack over the same ring slot.
                if (const auto ec = grid.prepareGpuVisibility(context)) {
                    return ec;
                }
                return grid.prepareGpuParticles(context);
            };
            // Production order (plan §3): particles (opaque then optical) ->
            // terrain opaque -> gas -> terrain translucent. The vacuum particle
            // therefore has to survive both terrain passes, and the particle in
            // the opaque cell can only stay hidden through the per-pixel
            // opaque-terrain hard clip.
            const auto draw = [&](const DrawContext &context) -> std::error_code {
                if (const auto ec = grid.renderSolidLiquidParticles(context)) {
                    return ec;
                }
                if (const auto ec = grid.renderGpuIndirect(context)) {
                    return ec;
                }
                if (const auto ec = grid.renderGasParticles(context)) {
                    return ec;
                }
                return grid.renderTranslucent(context);
            };

            DrawSubmission submission{"grid vacuum clip", DrawCallback{draw}};
            submission.m_viewport = viewport;
            submission.m_scissor = scissor;
            PPR_TEST_ASSERT(not renderer.renderToTexture(*target, {submission}, {}, PreRenderCallback{prepare}));
            grid.notifyFrameSubmitted(renderer.lastSubmittedFenceValue());
            PPR_TEST_ASSERT(not renderer.waitOnHost());
            return readTexture(fixture.device(), *target);
        };

        publish(std::nullopt);
        const TextureReadback baseline = capture();
        publish(float2{2.5f, 1.5f});
        const TextureReadback vacuum = capture();
        publish(float2{1.5f, 2.5f});
        const TextureReadback occupied = capture();

        const auto pixel = [](const TextureReadback &image, const u32 x, const u32 y) {
            return reinterpret_cast<const u16 *>(image.m_bytes.data() + y * image.m_row_pitch + x * 8u);
        };
        const auto different_pixels = [&](const TextureReadback &left, const TextureReadback &right) {
            u32 count = 0u;
            for (u32 y = 0u; y < 64u; ++y) {
                for (u32 x = 0u; x < 64u; ++x) {
                    const auto *const a = pixel(left, x, y);
                    const auto *const b = pixel(right, x, y);
                    if (a[0u] != b[0u] or a[1u] != b[1u] or a[2u] != b[2u] or a[3u] != b[3u]) {
                        ++count;
                    }
                }
            }
            return count;
        };

        const u32 vacuum_changed = different_pixels(vacuum, baseline);
        const u32 occupied_changed = different_pixels(occupied, baseline);
        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL particle_terrain_clip vacuum_pixels={} occupied_pixels={} draws={}",
                vacuum_changed, occupied_changed, grid.observations().m_draw_commands));
        // A particle in an excavated vacuum cell survives the terrain draw.
        PPR_TEST_ASSERT(vacuum_changed > 0u);
        // A particle under an opaque terrain cell is clipped, and terrain wins.
        PPR_TEST_ASSERT(occupied_changed == 0u);
    };

    PPR_UNIT_TEST (particle_shapes_share_one_support_bound) {
        // Arithmetic probe for the revised (P1) geometry: 2.0x support, the C1
        // falloff, the 3.0625 r_m cull margin that also covers the retained 1.5x
        // flow stretch, and the saturated seeded radial falloff (final solid
        // coverage is conditional; see particle_plateau_coverage_is_conditional).
        // The same constants are mirrored in grid_particle_native.slang for the
        // draw geometry and the prepare cull.
        constexpr double r_cell = 0.28209479177387814; // sqrt(0.25/pi) for volume 0.25
        const float rounded_support = gridParticleRoundedSquareSupport(
            kGridParticleSolidHalfExtentScale, kGridParticleSolidCornerScale);

        PPR_TEST_ASSERT(kGridParticleSupportScale == 2.0f);
        PPR_TEST_ASSERT(kGridParticleCullRadiusScale == 3.0625f);
        PPR_TEST_ASSERT(kGridParticleStretchCap == 1.5f);
        PPR_TEST_ASSERT(kGridParticleCoverageFloor == 1.0f / 255.0f);
        PPR_TEST_ASSERT(std::abs(rounded_support - kGridParticleSupportScale) <= 1.0e-4f);
        // max(support, kStretchCap * support) = 3.000 r <= 3.0625 r.
        PPR_TEST_ASSERT(rounded_support * kGridParticleStretchCap <= kGridParticleCullRadiusScale);
        // Cell units at the reference radius: a = 0.4305 cell, c = 0.1076 cell.
        PPR_TEST_ASSERT(std::abs(double(kGridParticleSolidHalfExtentScale) * r_cell - 0.4305) <= 5.0e-4);
        PPR_TEST_ASSERT(std::abs(double(kGridParticleSolidCornerScale) * r_cell - 0.1076) <= 5.0e-4);

        // The falloff is C1: value 1 up to R_in = (2/3) R, 0 at R and beyond, the
        // half-coverage contour at rho = 5/6, and (numerically) zero slope at
        // both joins.
        constexpr float epsilon = 1.0e-4f;
        const float inner = kGridParticleFalloffInnerFraction;
        PPR_TEST_ASSERT(gridParticleFalloff(0.0f) == 1.0f);
        PPR_TEST_ASSERT(gridParticleFalloff(inner) == 1.0f);
        PPR_TEST_ASSERT(std::abs(gridParticleFalloff(5.0f / 6.0f) - 0.5f) <= 1.0e-4f);
        PPR_TEST_ASSERT(gridParticleFalloff(1.0f) == 0.0f);
        PPR_TEST_ASSERT(gridParticleFalloff(1.5f) == 0.0f);
        PPR_TEST_ASSERT((gridParticleFalloff(inner) - gridParticleFalloff(inner + epsilon)) / epsilon <= 1.0e-2f);
        PPR_TEST_ASSERT((gridParticleFalloff(1.0f) - gridParticleFalloff(1.0f - epsilon)) / epsilon <= 1.0e-2f);

        // Coverage is now a shading quantity consumed by the premultiplied blend,
        // not a discard test: the opaque class composites this same continuous
        // falloff, so the old 0.5 contour is half coverage rather than a cut. Only
        // the negligible-fringe floor is discarded, which keeps the radial falloff
        // plateau, the support and the shape exactly where they were.
        const auto opaque_coverage = [](const float rho) { return gridParticleFalloff(rho); };
        PPR_TEST_ASSERT(std::abs(opaque_coverage(5.0f / 6.0f) - 0.5f) <= 1.0e-4f);
        PPR_TEST_ASSERT(opaque_coverage(5.0f / 6.0f) > kGridParticleCoverageFloor);
        PPR_TEST_ASSERT(opaque_coverage(0.0f) == 1.0f);
        PPR_TEST_ASSERT(opaque_coverage(inner) == 1.0f);
        PPR_TEST_ASSERT(opaque_coverage(1.0f) == 0.0f);
        PPR_TEST_ASSERT(opaque_coverage(1.5f) == 0.0f);
        PPR_TEST_ASSERT(opaque_coverage(1.0f) <= kGridParticleCoverageFloor);
        PPR_TEST_ASSERT(kGridParticleCoverageFloor > 0.0f);
        float previous_coverage = opaque_coverage(inner);
        for (u32 step = 1u; step <= 96u; ++step) {
            const float rho = static_cast<float>(double(inner) + (1.0 - double(inner)) * double(step) / 96.0);
            const float coverage = opaque_coverage(rho);
            PPR_TEST_ASSERT(coverage < previous_coverage);
            previous_coverage = coverage;
        }
        PPR_TEST_ASSERT(previous_coverage == 0.0f);

        // The seeded interior's radial falloff is saturated: the axis midpoint
        // (0.25 cell) and the diagonal midpoint / cell centre (0.3536 cell) are all
        // inside R_in, so a packed body is fully opaque in the radial term by
        // construction (plan §5.2). The final solid composite additionally
        // multiplies the SDF rim and the terrain complement, so its opacity is
        // exactly 1 only in the conditional regime above. Overlapping particles
        // union their coverages through the premultiplied One/InvSrcAlpha
        // composite, so "opaque" here means the union of these continuous coverage
        // terms, not a discard test.
        const double support_cell = double(kGridParticleSupportScale) * r_cell;
        const double rho_axis = 0.25 / support_cell;
        const double rho_diagonal = 0.35355339059327373 / support_cell;
        PPR_TEST_ASSERT(rho_axis < double(kGridParticleFalloffInnerFraction));
        PPR_TEST_ASSERT(rho_diagonal < double(kGridParticleFalloffInnerFraction));
        const auto union_coverage = [](const double rho, const u32 contributors) {
            const double coverage = double(gridParticleFalloff(static_cast<float>(rho)));
            return 1.0 - std::pow(1.0 - coverage, double(contributors));
        };
        PPR_TEST_ASSERT(union_coverage(rho_axis, 2u) >= 0.99);
        PPR_TEST_ASSERT(union_coverage(rho_diagonal, 2u) >= 0.98);
        PPR_TEST_ASSERT(union_coverage(rho_diagonal, 4u) >= 0.99);

        // Rejected variants, kept recorded: the rounded square of half-extent
        // a = R (keeping the 0.25 a corner fraction) needs 2.621x the physical
        // radius, the unrounded square of a = R needs 2.828x and the earlier
        // 0.5-cell half-extent square needs 2.507x. All three exceed the adopted
        // 2.0 r support, so they remain rejected as appearance (a bigger
        // silhouette and more fill); the raised 3.0625 r margin no longer makes
        // them cull-infeasible, which is why they are compared to the support.
        PPR_TEST_ASSERT(
            gridParticleRoundedSquareSupport(
                kGridParticleSupportScale, kGridParticleSolidCornerFraction * kGridParticleSupportScale) >
            kGridParticleSupportScale);
        PPR_TEST_ASSERT(1.41421356237309505f * kGridParticleSupportScale > kGridParticleSupportScale);
        PPR_TEST_ASSERT(1.41421356237309505f * 0.5f / float(r_cell) > kGridParticleSupportScale);

        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL particle_shape support={:.4f} stretched={:.4f} cull={:.4f} "
                "half_extent={:.4f} corner={:.4f} half_extent_cell={:.4f} corner_cell={:.4f} "
                "rho_axis={:.4f} rho_diagonal={:.4f}",
                rounded_support, rounded_support * kGridParticleStretchCap, kGridParticleCullRadiusScale,
                kGridParticleSolidHalfExtentScale, kGridParticleSolidCornerScale,
                double(kGridParticleSolidHalfExtentScale) * r_cell, double(kGridParticleSolidCornerScale) * r_cell,
                rho_axis, rho_diagonal));
    };

    // ------------------------------------------------------------------
    // optional flow cue: direction and magnitude mapping
    // ------------------------------------------------------------------
    // Mirrors grid_tiles.slang `particleVertex`/`particleFlowLocal`; the bounded
    // constants are the CPU twin's kGridParticleStretchCap/kGridParticleMotionReference
    // (App.Renderer.GridPass.cppm) and the shader's kParticleStretchCap/
    // kParticleMotionReference (grid_particle_native.slang). This mirror exists so
    // the mapping, the anisotropic metric and the support bound are asserted
    // without a screenshot. The row's own motion sanitising (finite components,
    // each clamped to +-64 cells/step) is asserted on the packed GPU row by
    // presentation_motion_half_round_trips_and_clamps and the projection entry,
    // and is not repeated here.

    struct FlowPoint {
        double x{};
        double y{};
    };

    // k = 1 + (cap - 1) * saturate(|motion| / reference), in native cells per
    // solver step. Exactly 1 at rest and exactly the cap at and beyond the
    // reference magnitude; the magnitude is illustrative, never a physical speed.
    [[nodiscard]] constexpr float gridParticleFlowStretch(const float motion_length) noexcept {
        const float ratio = motion_length / kGridParticleMotionReference;
        const float saturated = ratio < 0.0f ? 0.0f : (ratio > 1.0f ? 1.0f : ratio);
        return 1.0f + (kGridParticleStretchCap - 1.0f) * saturated;
    }

    // The metric divides the flow-axis coordinate by k and leaves the cross-flow
    // coordinate untouched, so the un-stretched distance and falloff evaluated at
    // the result describe the shape stretched by k along the flow. k == 1
    // returns the sample point untouched, so the zero-motion shape is today's.
    [[nodiscard]] constexpr FlowPoint gridParticleFlowLocal(
        const FlowPoint local, const FlowPoint flow, const float stretch) noexcept {
        if (not (stretch > 1.0f))
            return local;

        const double along = local.x * flow.x + local.y * flow.y;
        const double shrink = 1.0 - 1.0 / static_cast<double>(stretch);
        return {local.x - shrink * along * flow.x, local.y - shrink * along * flow.y};
    }

    // The inverse metric: where a point of the un-stretched shape lands after the
    // stretch. The largest such reach is the half-extent the billboard and the
    // cull margin have to cover.
    [[nodiscard]] constexpr FlowPoint gridParticleFlowWorld(
        const FlowPoint base, const FlowPoint flow, const float stretch) noexcept {
        if (not (stretch > 1.0f))
            return base;

        const double along = base.x * flow.x + base.y * flow.y;
        const double grow = static_cast<double>(stretch) - 1.0;
        return {base.x + grow * along * flow.x, base.y + grow * along * flow.y};
    }

    [[nodiscard]] FlowPoint gridParticleDirection(const double angle) noexcept {
        return {std::cos(angle), std::sin(angle)};
    }

    [[nodiscard]] double gridParticleLength(const FlowPoint point) noexcept {
        return std::sqrt(point.x * point.x + point.y * point.y);
    }

    // rho = |point| / R of the un-stretched falloff, for a point in r units.
    [[nodiscard]] double gridParticleRho(const FlowPoint point) noexcept {
        return gridParticleLength(point) / double(kGridParticleSupportScale);
    }

    // ------------------------------------------------------------------
    // per-cell winner-motion sampler mirror (Task 4 continuity fix)
    // ------------------------------------------------------------------
    // Mirrors grid_tiles.slang particleCellMotion/particleFlowMotion. A field
    // answers one lattice value and whether that cell has a committed winner; the
    // sample is the four dual-lattice bilinear weights renormalised over the taps
    // that have a winner above W = 1/4, and the continuous blend toward the row's
    // own motion below it. Out-of-range is empty, so a synthetic field must bound
    // itself the way the shader's kResolvedEdge guard does.
    template<typename Field>
    [[nodiscard]] FlowPoint gridParticleFlowMotion(
        const Field &field, const FlowPoint cell_position, const FlowPoint row_motion) noexcept {
        const FlowPoint p{cell_position.x - 0.5, cell_position.y - 0.5};
        const int base_x = static_cast<int>(std::floor(p.x));
        const int base_y = static_cast<int>(std::floor(p.y));
        const double fraction_x = p.x - double(base_x);
        const double fraction_y = p.y - double(base_y);
        const std::array<FlowPoint, 4u> taps{{{0.0, 0.0}, {1.0, 0.0}, {0.0, 1.0}, {1.0, 1.0}}};
        const std::array<double, 4u> weights{
            (1.0 - fraction_x) * (1.0 - fraction_y), fraction_x * (1.0 - fraction_y), (1.0 - fraction_x) * fraction_y,
            fraction_x * fraction_y};

        FlowPoint sum{};
        double weight = 0.0;
        for (u32 tap = 0u; tap < taps.size(); ++tap) {
            FlowPoint value{};
            if (not field(base_x + static_cast<int>(taps[tap].x), base_y + static_cast<int>(taps[tap].y), value))
                continue;

            sum.x += weights[tap] * value.x;
            sum.y += weights[tap] * value.y;
            weight += weights[tap];
        }

        // Continuous extension below W = 1/4, exactly as the shader ships it: the
        // two branches agree at W = 1/4 (4S) and the sample is the row's own
        // motion at W = 0 for every approach.
        if (weight < 0.25)
            return {
                4.0 * sum.x + (1.0 - 4.0 * weight) * row_motion.x, 4.0 * sum.y + (1.0 - 4.0 * weight) * row_motion.y};

        return {sum.x / weight, sum.y / weight};
    }

    // The negative control the continuity probes are measured against: the same
    // dual-lattice window, but a box filter - the window's base cell is held for the
    // whole sub-cell window instead of being interpolated. It is piecewise constant
    // with a step at every window edge, cell_position = k + 0.5, which is the cell
    // centre the field is published at and therefore where the old per-cell cue
    // stepped. The probes assert that it violates their bounds while the shipped
    // sampler satisfies them, so a bound both pass would be no evidence about the
    // interpolation. Absence keeps the bilinear sampler's own fallback: the entry's
    // row motion.
    template<typename Field>
    [[nodiscard]] FlowPoint gridParticleFlowMotionBox(
        const Field &field, const FlowPoint cell_position, const FlowPoint row_motion) noexcept {
        const FlowPoint p{cell_position.x - 0.5, cell_position.y - 0.5};
        FlowPoint value{};
        if (field(static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y)), value))
            return value;

        return row_motion;
    }

    // Worst stretch and direction step of one sampler over a sweep of neighbouring
    // sample pairs. Both continuity sweeps and their box control use it, so the
    // bound is applied to the sampler under test rather than to one hard-coded
    // interpolation.
    struct ContinuityProbe {
        double m_stretch{};
        double m_angle{};
    };

    // The cue's two outputs from one sampled motion: the elongation from the
    // sampled magnitude and the unit flow direction in the (world = quad) frame.
    [[nodiscard]] float gridParticleSampleStretch(const FlowPoint motion) noexcept {
        return gridParticleFlowStretch(static_cast<float>(gridParticleLength(motion)));
    }

    [[nodiscard]] double gridParticleFlowAngle(const FlowPoint motion) noexcept {
        return std::atan2(motion.y, motion.x);
    }

    // Smallest rotation between two directions, in radians.
    [[nodiscard]] double gridParticleAngleDifference(const double left, const double right) noexcept {
        double difference = std::fmod(left - right + kPi, 2.0 * kPi);
        if (difference < 0.0)
            difference += 2.0 * kPi;

        return std::abs(difference - kPi);
    }

    // ------------------------------------------------------------------
    // tangent-aligned contour mirror (Task 11 increment 1)
    // ------------------------------------------------------------------
    // Mirrors grid_tiles.slang particleSurfaceTangent/particleTangentFalloff and
    // the fixed constants in grid_particle_native.slang (CPU twin above). The
    // tangent frame here is the world-plane frame, which is the quad frame for the
    // top-down fixture; the shader projects the same gradient with right/up. The
    // sampled field is the committed per-cell Q16 fill, so a field function here
    // returns cell volume (1.0 = one full cell) and a boundary ramps through the
    // 0.25 quanta instead of switching one bit.

    // Mirror of the energy gate in grid_particle_native.slang: the cue's strength
    // is smoothstep(low, high, |grad|) in the unnormalised Sobel units, so a
    // genuine surface keeps full bias and the ambiguous fields fade to the round
    // contour.
    [[nodiscard]] constexpr double gridParticleTangentStrength(const double gradient_magnitude) noexcept {
        const double low = double(kGridParticleTangentConfidenceLow);
        const double high = double(kGridParticleTangentConfidenceHigh);
        const double t = std::clamp((gradient_magnitude - low) / (high - low), 0.0, 1.0);
        return t * t * (3.0 - 2.0 * t);
    }

    // Separable 3x3 Sobel of the bilinearly reconstructed field: the exact weight
    // sequence grid_tiles.slang particleSurfaceTangent ships (the Sobel difference
    // on one axis and the 1-2-1 smoothing on the other, over one 4x4 cell window).
    // Returns false when the field is flat around the centre: the dense interior,
    // the empty exterior and an isolated particle then have no surface direction.
    // `gradient_magnitude`, when requested, reports the pre-normalisation length;
    // `strength` reports the energy gate on it.
    template<typename Field>
    [[nodiscard]] bool gridParticleSurfaceTangent(
        const Field &field, const FlowPoint centre, FlowPoint &tangent, double *const gradient_magnitude = nullptr,
        double *const strength = nullptr) noexcept {
        const FlowPoint p{centre.x - 0.5, centre.y - 0.5};
        const int base_x = static_cast<int>(std::floor(p.x));
        const int base_y = static_cast<int>(std::floor(p.y));
        const double bx0 = 1.0 - (p.x - double(base_x));
        const double bx1 = p.x - double(base_x);
        const double by0 = 1.0 - (p.y - double(base_y));
        const double by1 = p.y - double(base_y);
        const std::array<double, 4u> difference_x{-bx0, -bx1, bx0, bx1};
        const std::array<double, 4u> smooth_x{bx0, 2.0 * bx0 + bx1, bx0 + 2.0 * bx1, bx1};
        const std::array<double, 4u> difference_y{-by0, -by1, by0, by1};
        const std::array<double, 4u> smooth_y{by0, 2.0 * by0 + by1, by0 + 2.0 * by1, by1};

        double gradient_x = 0.0;
        double gradient_y = 0.0;
        for (u32 row = 0u; row < 4u; ++row) {
            double window_difference = 0.0;
            double window_smooth = 0.0;
            for (u32 column = 0u; column < 4u; ++column) {
                const double value = field(base_x - 1 + static_cast<int>(column), base_y - 1 + static_cast<int>(row));
                window_difference += value * difference_x[column];
                window_smooth += value * smooth_x[column];
            }

            gradient_x += smooth_y[row] * window_difference;
            gradient_y += difference_y[row] * window_smooth;
        }

        const double squared = gradient_x * gradient_x + gradient_y * gradient_y;
        const double epsilon = double(kGridParticleTangentGradientEpsilon);
        const double magnitude = std::sqrt(squared);
        if (gradient_magnitude != nullptr)
            *gradient_magnitude = magnitude;
        if (strength != nullptr)
            *strength = gridParticleTangentStrength(magnitude);

        if (squared <= epsilon * epsilon)
            return false;

        const double inverse = 1.0 / magnitude;
        const FlowPoint normal{-gradient_x * inverse, -gradient_y * inverse};
        tangent = {-normal.y, normal.x};
        return true;
    }

    // Angular contour radius k(theta) in rho units, exactly as the shader ships
    // it: the raw expression clamped to the band's defensive limits. The clamp is
    // inert for b = 0.3999 except within about 0.76 degrees of the normal
    // direction, so `gridParticleTangentContourUnclamped` below is what the
    // endpoint assertions compare against.
    [[nodiscard]] constexpr float gridParticleTangentContourUnclamped(
        const float cos_squared, const float strength = 1.0f) noexcept {
        const float bias = kGridParticleTangentContourBias * strength;
        return kGridParticleTangentContourScale * (1.0f + bias * (cos_squared - 0.5f));
    }

    [[nodiscard]] constexpr float gridParticleTangentContour(
        const float cos_squared, const float strength = 1.0f) noexcept {
        const float raw = gridParticleTangentContourUnclamped(cos_squared, strength);
        return raw < kGridParticleTangentContourMinimum ? kGridParticleTangentContourMinimum
                                                        : (raw > 1.0f ? 1.0f : raw);
    }

    // Mirror of grid_tiles.slang particleTangentFalloff: the resting C1 smoothstep
    // evaluated on a Mobius-warped band coordinate whose 0.5 crossing is k(theta).
    // The plateau and the support are returned explicitly, exactly as the shader
    // does, so the mirror is total on the same domain; the plateau here is the
    // radial falloff plateau, before the solid rim and terrain complement
    // multiply it. `strength` scales b toward 0 (the energy gate); at 0 the
    // contour is exactly k0.
    [[nodiscard]] double gridParticleTangentFalloff(
        const FlowPoint local, const FlowPoint tangent, const double strength = 1.0) noexcept {
        const double rho = gridParticleRho(local);
        const double inner = double(kGridParticleFalloffInnerFraction);
        const double closure = double(kGridParticleTangentJoinClosure);
        if (rho <= inner + closure)
            return 1.0;
        if (rho >= 1.0 - closure)
            return 0.0;

        const double length = gridParticleLength(local);
        const double direction_x = local.x / length;
        const double direction_y = local.y / length;
        const double cosine = direction_x * tangent.x + direction_y * tangent.y;
        const double contour = double(
            gridParticleTangentContour(static_cast<float>(cosine * cosine), static_cast<float>(strength)));
        const double u = (rho - inner) / (1.0 - inner);
        const double u_contour = (contour - inner) / (1.0 - inner);
        const double warp = 2.0 - 1.0 / u_contour;
        const double w = u * (1.0 - warp) / (1.0 - warp * u);
        const double t = 1.0 - w;
        return t * t * (3.0 - 2.0 * t);
    }

    // Mirror of the fragment's shaded falloff: the resting value everywhere
    // outside the open band or without a tangent, the warped contour only inside
    // it. This is the contract that keeps the plateau and the support byte-exact.
    [[nodiscard]] float gridParticleShadedFalloff(
        const FlowPoint local, const FlowPoint tangent, const bool has_tangent, const float strength = 1.0f) noexcept {
        const float resting = gridParticleFalloff(static_cast<float>(gridParticleRho(local)));
        if (not has_tangent or not (resting > 0.0f) or not (resting < 1.0f))
            return resting;

        return static_cast<float>(gridParticleTangentFalloff(local, tangent, double(strength)));
    }

    // Mirror of grid_tiles.slang particleSolidDistance: the signed distance to
    // the solid rounded-square boundary, in the same r units as `local`. Negative
    // inside the solid, zero on the boundary and positive outside it.
    [[nodiscard]] float gridParticleSolidDistance(const FlowPoint local) noexcept {
        const float half_extent = kGridParticleSolidInnerHalfExtentScale;
        const float offset_x = static_cast<float>(std::abs(local.x)) - half_extent;
        const float offset_y = static_cast<float>(std::abs(local.y)) - half_extent;
        const float outside_x = (std::max) (offset_x, 0.0f);
        const float outside_y = (std::max) (offset_y, 0.0f);
        return std::sqrt(outside_x * outside_x + outside_y * outside_y) +
               (std::min) ((std::max) (offset_x, offset_y), 0.0f) -
               kGridParticleSolidCornerScale;
    }

    // Mirror of the fragment's analytic rim: a liquid keeps the full shape term,
    // while a solid crosses from 1 to 0 over `rim_width` (the pixel footprint in
    // local r units, derived from ddx/ddy in the shader). On the SDF boundary the
    // coverage is exactly 0.5, which is what replaces the old hard cut.
    [[nodiscard]] float gridParticleShapeCoverage(
        const FlowPoint local, const float rim_width, const bool liquid) noexcept {
        if (liquid)
            return 1.0f;

        const float width = (std::max) (rim_width, 1.0e-6f);
        const float coverage = 0.5f - gridParticleSolidDistance(local) / width;
        return (std::min) ((std::max) (coverage, 0.0f), 1.0f);
    }

    // The fragment's full pre-blend coverage, mirrored in the shader's float
    // order: falloff * shape_coverage * (1 - terrain_coverage). The plateau, the
    // support and a liquid's numbers are asserted through this, not through a
    // rewritten expression.
    [[nodiscard]] float gridParticleEdgeCoverage(
        const FlowPoint local, const FlowPoint tangent, const bool has_tangent, const bool liquid,
        const float rim_width, const float terrain_coverage) noexcept {
        const float falloff = gridParticleShadedFalloff(local, tangent, has_tangent);
        const float shape = gridParticleShapeCoverage(local, rim_width, liquid);
        return falloff * shape * (1.0f - terrain_coverage);
    }

    // ------------------------------------------------------------------
    // screen-space particle density kernel and composite mirror
    // ------------------------------------------------------------------
    // Independent restatement of grid_particle_density.slang (plan sections
    // 10.2, 10.3 and 10.9). It carries its own literals instead of the shader's
    // names, so a drifted shader constant fails the probes below rather than
    // matching itself. D = sum w is a reconstructed reference-area concentration
    // per unit world area sampled in screen space: under a uniform rest
    // distribution its spatial mean is one. It is not particles per pixel, not
    // the liquid solver variable J, not the mass density of mixed substances and
    // not an optical thickness along a 3D ray.

    // Revision 5 calibration: one shared surface radius for both phases, the
    // retained threshold, and the neutral rest-lattice closure numbers.
    inline constexpr double kDensityDirectSupportScale = 2.0;
    inline constexpr double kDensitySurfaceRadiusScale = 1.5259485586443693;
    inline constexpr double kDensityPlateau = 2.0 / 3.0;
    inline constexpr double kDensityBandFraction = 1.0 / 3.0;
    // The profile integrates to 0.7*pi over the unit q-disk; A_2 = 1/(0.7 pi) =
    // 10/(7 pi) is what makes A_2*0.7 pi = 1. The amplitude of
    // w = V*A_2*sqrt(det M)/R^2*s is V/R^2 = pi/c^2 times A_2, i.e. pi*A_2/c^2 =
    // (10/7)/c^2. The profile's 0.7 is already inside A_2 and must not appear as a
    // factor of the amplitude (that underweights every splat by pi/0.7 = 4.488).
    inline constexpr double kDensityProfileIntegral = 0.7;
    inline constexpr double kDensitySecondMoment = 10.0 / (7.0 * kPi);
    inline constexpr double kDensityAmplitude = kPi *
                                                kDensitySecondMoment /
                                                (kDensitySurfaceRadiusScale * kDensitySurfaceRadiusScale);
    inline constexpr double kDensityStretchCap = 1.5;
    inline constexpr double kDensityBiasCap = 0.3999;
    inline constexpr double kDensityThreshold = 5.0 / 28.0;
    inline constexpr double kDensityEpsilon = 1.0e-6;
    inline constexpr double kDensitySupportClosure = 1.0e-6;
    inline constexpr double kDensityCoverageFloor = 1.0 / 255.0;
    inline constexpr double kDensityQuadratureOffset = 0.25;
    inline constexpr double kDensityCullRadiusScale = 3.0625;
    inline constexpr double kDensityDepthCeiling = 0.9999999403953552;

    // The anisotropic metric decoded from the existing 4-byte orientation word:
    // b = 0.3999 * bias8/255, k = 1 + 0.5 * stretch8/255, eta = (1-b/2)/(1+b/2)
    // and det(A) = k * eta. eta <= 1 and k <= 1.5 keep every oriented support
    // inside max(k, eta) * R_surface = 2.288923 r, under the retained 3.0625 r
    // cull (which still covers the direct solid's 3.000 r silhouette).
    struct DensityKernel {
        Double2 m_axis{1.0, 0.0};
        double m_stretch{1.0};
        double m_ratio{1.0};
    };

    [[nodiscard]] constexpr DensityKernel densityKernel(
        const Double2 axis, const double bias_fraction, const double stretch_fraction) noexcept {
        const double bias = kDensityBiasCap * std::clamp(bias_fraction, 0.0, 1.0);
        const double stretch = std::clamp(stretch_fraction, 0.0, 1.0);
        return {axis, 1.0 + (kDensityStretchCap - 1.0) * stretch, (1.0 - 0.5 * bias) / (1.0 + 0.5 * bias)};
    }

    // s(t) = t^2 (3 - 2t), the shared C1 ramp.
    [[nodiscard]] constexpr double densityRamp(const double t) noexcept {
        return t * t * (3.0 - 2.0 * t);
    }

    // rho = |inverse(A) (x - p) / R| for an offset already divided by R.
    [[nodiscard]] double densityRho(const Double2 offset, const DensityKernel kernel) noexcept {
        const double along = (offset.x * kernel.m_axis.x + offset.y * kernel.m_axis.y) / kernel.m_stretch;
        const double across = (-offset.x * kernel.m_axis.y + offset.y * kernel.m_axis.x) / kernel.m_ratio;
        return std::sqrt(along * along + across * across);
    }

    // w(x) at one world-plane offset in support-radius units. Exactly 0 at and
    // beyond the support: the explicit closure covers an anisotropic transform
    // that arrives a few ulps inside R, the same role the orientation cue's join
    // closure plays for the direct silhouette.
    [[nodiscard]] double densityWeight(const Double2 offset, const DensityKernel kernel) noexcept {
        const double rho = densityRho(offset, kernel);
        if (not (rho < 1.0 - kDensitySupportClosure))
            return 0.0;

        const double t = std::clamp((1.0 - rho) / kDensityBandFraction, 0.0, 1.0);
        return densityRamp(t) * kDensityAmplitude / (kernel.m_stretch * kernel.m_ratio);
    }

    // The same weight for a world-plane offset and particle radius in native cells.
    [[nodiscard]] double densityWeightCells(
        const Double2 offset_cells, const double radius_cells, const DensityKernel kernel) noexcept {
        const double support = kDensitySurfaceRadiusScale * radius_cells;
        return densityWeight({offset_cells.x / support, offset_cells.y / support}, kernel);
    }

    // The four fixed 2x2 quadrature positions of one density pixel.
    [[nodiscard]] constexpr Double2 densityQuadratureOffset(const u32 index) noexcept {
        return {
            (index & 1u) == 0u ? -kDensityQuadratureOffset : kDensityQuadratureOffset,
            (index & 2u) == 0u ? -kDensityQuadratureOffset : kDensityQuadratureOffset};
    }

    // B = [D > theta]: binary body occupancy at one scene sample, restated from
    // grid_particle_density.slang particleDensityOccupancy. There is no per-sample
    // ramp, so a sample is occupied or it is not; a resolved pixel's alpha is the
    // fraction of its covered samples times the material's own opacity, and the
    // contour's spatial smoothness belongs to the reconstruction that consumes the
    // individual samples.
    [[nodiscard]] double densityOccupancy(const double density) noexcept {
        return density > kDensityThreshold ? 1.0 : 0.0;
    }

    // One stored surface texel: the winning label plus the Q/G sums it owns. The
    // CPU mirror keeps the shader's exact channel meanings: Q = (sum w * base.rgb,
    // sum w) and G = (sum w * normal.xyz, sum w * alpha).
    struct SurfaceTexel {
        u32 m_label{};
        Double3 m_base{};
        double m_density{};
        Double3 m_normal{};
        double m_alpha{};
        // The palette class of the label this texel stored (opaque = false).
        bool m_optical{};
    };

    // One stored texel built from this texel's own accepted contributions:
    // Q = (sum w * base, sum w) and G = (sum w * normal, sum w * alpha), exactly
    // the channel meanings the accumulation writes.
    [[nodiscard]] SurfaceTexel surfaceTexel(
        const u32 label, const double weight, const Double3 base, const Double3 normal, const double alpha,
        const bool optical) noexcept {
        return {
            label, Double3{base.x * weight, base.y * weight, base.z * weight}, weight,
            Double3{normal.x * weight, normal.y * weight, normal.z * weight}, alpha * weight, optical};
    }

    struct SurfaceReconstruction {
        bool m_valid{};
        u32 m_material{};
        double m_density{};
        Double3 m_base{};
        Double3 m_normal{};
        double m_alpha{};
        double m_derivative_x{};
        double m_derivative_y{};
    };

    // Independent restatement of grid_tiles.slang particleSurfaceReconstruct:
    // integer four-tap categorical gather with the analytic quotient derivative.
    // The label is never filtered, an empty tap keeps its weight as air, a foreign
    // nonempty tap keeps no weight and contributes no value, and the returned
    // material is always the nearest own label (never a mixture). The composite
    // class must be the winning label's own class.
    [[nodiscard]] SurfaceReconstruction densityReconstruct(
        const std::array<SurfaceTexel, 4u> &taps, const SurfaceTexel &nearest, const double fx, const double fy,
        const bool composite_optical) noexcept {
        SurfaceReconstruction result{};
        if (nearest.m_label == 0u)
            return result;
        if (nearest.m_optical != composite_optical)
            return result;

        const std::array<double, 4u> beta{(1.0 - fx) * (1.0 - fy), fx * (1.0 - fy), (1.0 - fx) * fy, fx * fy};
        const std::array<double, 4u> beta_x{-(1.0 - fy), 1.0 - fy, -fy, fy};
        const std::array<double, 4u> beta_y{-(1.0 - fx), -fx, 1.0 - fx, fx};

        double air = 0.0;
        double air_x = 0.0;
        double air_y = 0.0;
        double density = 0.0;
        double density_x = 0.0;
        double density_y = 0.0;
        Double3 base{};
        Double3 normal{};
        double alpha = 0.0;
        for (u32 tap = 0u; tap < 4u; ++tap) {
            const SurfaceTexel &texel = taps[tap];
            const bool matching = texel.m_label == nearest.m_label;
            const bool empty = texel.m_label == 0u;
            const double keep = matching or empty ? 1.0 : 0.0;
            air += beta[tap] * keep;
            air_x += beta_x[tap] * keep;
            air_y += beta_y[tap] * keep;
            if (not matching)
                continue;

            density += beta[tap] * texel.m_density;
            density_x += beta_x[tap] * texel.m_density;
            density_y += beta_y[tap] * texel.m_density;
            base.x += texel.m_base.x * beta[tap];
            base.y += texel.m_base.y * beta[tap];
            base.z += texel.m_base.z * beta[tap];
            normal.x += texel.m_normal.x * beta[tap];
            normal.y += texel.m_normal.y * beta[tap];
            normal.z += texel.m_normal.z * beta[tap];
            alpha += beta[tap] * texel.m_alpha;
        }

        if (not (air > 0.0) or not (density > 0.0))
            return result;

        // F = U/Z, then the attribute means divide by the same density channel.
        result.m_valid = true;
        result.m_material = nearest.m_label;
        result.m_density = density / air;
        result.m_base = Double3{base.x / density, base.y / density, base.z / density};
        const Double3 normal_vector{normal.x / density, normal.y / density, normal.z / density};
        const double length_squared = normal_vector.x * normal_vector.x +
                                      normal_vector.y * normal_vector.y +
                                      normal_vector.z * normal_vector.z;
        result.m_normal = length_squared > 1.0e-12 and normal_vector.z > 0.0
            ? Double3{normal_vector.x / std::sqrt(length_squared), normal_vector.y / std::sqrt(length_squared),
                  normal_vector.z / std::sqrt(length_squared)}
            : Double3{0.0, 0.0, 1.0};
        result.m_alpha = alpha / density;
        result.m_derivative_x = (density_x * air - density * air_x) / (air * air);
        result.m_derivative_y = (density_y * air - density * air_y) / (air * air);
        return result;
    }

    // Independent restatement of the composite tail: form the straight response
    // colour H and alpha from the reconstructed attributes, then premultiply once
    // and scale by the geometric coverage. The response itself is the shipped
    // material helper; this mirror only proves the coverage/premultiplication
    // algebra and the divide guard.
    struct SurfaceComposite {
        bool m_visible{};
        Double3 m_premultiplied{};
        double m_alpha{};
    };

    [[nodiscard]] SurfaceComposite densityComposite(
        const SurfaceReconstruction &reconstruction, const Double3 straight_color, const double coverage,
        const bool optical) noexcept {
        if (not reconstruction.m_valid or not (reconstruction.m_density > kDensityEpsilon))
            return {};
        if (not (coverage > kDensityCoverageFloor))
            return {};

        const double alpha = optical ? std::clamp(reconstruction.m_alpha, 0.0, 1.0) : 1.0;
        const double scale = coverage * alpha;
        return {
            true, Double3{straight_color.x * scale, straight_color.y * scale, straight_color.z * scale},
            coverage * alpha};
    }

    // Midpoint integral of one kernel over the plane in native cells. The square
    // extends 10% past the oriented support, so the truncation is exact.
    [[nodiscard]] double densityIntegral(
        const DensityKernel kernel, const double radius_cells, const u32 steps) noexcept {
        const double support = kDensitySurfaceRadiusScale * radius_cells;
        const double reach = 1.1 * (std::max) ((std::max) (kernel.m_stretch, kernel.m_ratio), 1.0) * support;
        const int count = static_cast<int>(steps);
        const double extent = 2.0 * reach;
        const double step = extent / double(count);
        double total = 0.0;
        for (int row = 0; row < count; ++row) {
            const double y = -0.5 * extent + (double(row) + 0.5) * step;
            for (int column = 0; column < count; ++column) {
                const double x = -0.5 * extent + (double(column) + 0.5) * step;
                total += densityWeight({x / support, y / support}, kernel);
            }
        }

        return total * step * step;
    }

    // The accumulation one density-pixel grid actually performs: average the four
    // quadrature offsets per pixel, then multiply by the pixel area. It approaches
    // the reference area only as the pixel shrinks, which is why its tolerance is
    // reported separately from the continuous integral.
    [[nodiscard]] double densityQuadratureIntegral(
        const DensityKernel kernel, const double radius_cells, const double pixel) noexcept {
        const double support = kDensitySurfaceRadiusScale * radius_cells;
        const double reach = 1.1 * (std::max) ((std::max) (kernel.m_stretch, kernel.m_ratio), 1.0) * support;
        const int count = static_cast<int>(std::ceil(2.0 * reach / pixel));
        const double half = 0.5 * double(count) * pixel;
        double total = 0.0;
        for (int row = 0; row < count; ++row) {
            const double y = -half + (double(row) + 0.5) * pixel;
            for (int column = 0; column < count; ++column) {
                const double x = -half + (double(column) + 0.5) * pixel;
                double quadrature = 0.0;
                for (u32 corner = 0u; corner < 4u; ++corner) {
                    const Double2 offset = densityQuadratureOffset(corner);
                    quadrature += densityWeight(
                        {(x + offset.x * pixel) / support, (y + offset.y * pixel) / support}, kernel);
                }

                total += 0.25 * quadrature * pixel * pixel;
            }
        }

        return total;
    }

    // Exact discrete expectation of the accumulated field for ONE isolated
    // particle: the operator grid_tiles.slang actually runs, evaluated in double.
    //
    //   * the splat quad is the screen-aligned square of half-extent
    //     R*conic_support + pad with pad = one density pixel, so the square always
    //     encloses the support circle (R*conic_support is its major semiaxis);
    //     only texels whose CENTRE lies inside that square rasterise;
    //   * a texel receives the label iff at least one quadrature point has
    //     rho < 1, which is exactly the condition for a nonzero averaged weight,
    //     so the label pass can never remove a positive contribution;
    //   * the stored density is (1/4)*sum_j w(centre + offset_j) with offsets
    //     (+/-1/4, +/-1/4) pixel, and the plane integral sums that over texels
    //     times the pixel area.
    //
    // Unlike the particle-centred grid in densityQuadratureIntegral this carries the
    // raster phase and the quad clip. The per-texel oracle additionally models the
    // rasteriser's sub-pixel vertex snapping (see RasterisedQuad); the INTEGRAL keeps
    // the ideal geometry because that snapping is a uniform radial scale of ~1e-4 per
    // quad, which changes the summed area by ~2e-4 relative -- an order below the
    // 1e-3/2e-3 integral bounds -- and averaging it into every texel would make the
    // integral oracle depend on the rasteriser grid rather than on the kernel.
    [[nodiscard]] double densityFieldIntegral(
        const DensityKernel kernel, const double radius_cells, const double pixel, const u32 raster,
        const double particle_raster_x, const double particle_raster_y, const double conic_support) noexcept {
        const double support = kDensitySurfaceRadiusScale * radius_cells;
        const double half_extent = support * conic_support + pixel;
        double total = 0.0;
        for (u32 y = 0u; y < raster; ++y) {
            const double cy = (double(y) + 0.5 - particle_raster_y) * pixel;
            if (std::abs(cy) > half_extent)
                continue;
            for (u32 x = 0u; x < raster; ++x) {
                const double cx = (double(x) + 0.5 - particle_raster_x) * pixel;
                if (std::abs(cx) > half_extent)
                    continue;
                double quadrature = 0.0;
                for (u32 corner = 0u; corner < 4u; ++corner) {
                    const Double2 offset = densityQuadratureOffset(corner);
                    quadrature += densityWeight(
                        {(cx + offset.x * pixel) / support, (cy + offset.y * pixel) / support}, kernel);
                }

                total += 0.25 * quadrature * pixel * pixel;
            }
        }

        return total;
    }

    // D at a world-plane point of the nominal rest lattice: reference volume
    // 0.25 cell, spacing 0.5 cell, neutral cues, summed over the neighbours that
    // reach the point.
    [[nodiscard]] double densityLatticeSample(const Double2 point) noexcept {
        constexpr double volume = 0.25;
        constexpr double spacing = 0.5;
        const double radius = std::sqrt(volume / kPi);
        const DensityKernel kernel = densityKernel({1.0, 0.0}, 0.0, 0.0);
        double density = 0.0;
        for (int y = -3; y <= 3; ++y) {
            for (int x = -3; x <= 3; ++x) {
                density += densityWeightCells(
                    {point.x - double(x) * spacing, point.y - double(y) * spacing}, radius, kernel);
            }
        }

        return density;
    }

    // ------------------------------------------------------------------
    // the documented world-space smoothing operator, evaluated on the CPU
    // ------------------------------------------------------------------
    // grid_tiles.slang's two smoothing entries are a separable label-masked
    // Gaussian: sigma = 0.15 world cell truncated at +/-3 sigma, the per-axis tap
    // count taken from the frame's own raster samples-per-cell, air keeping its
    // weight in the denominator and a foreign nonempty label leaving it. These
    // helpers restate that operator exactly, so a probe compares the GPU's two
    // shared-memory passes against the specification instead of a golden image.
    inline constexpr double kSurfaceFilterSigma = 0.15;

    [[nodiscard]] std::vector<double> surfaceFilterWeights(const double samples_per_cell, const u32 halo) {
        std::vector<double> weights(2u * halo + 1u, 0.0);
        for (u32 tap = 0u; tap < weights.size(); ++tap) {
            // The exponential is evaluated in WORLD CELLS, exactly as the shader:
            // a raster offset divided by samples per cell, then by sigma.
            const double cells = (double(tap) - double(halo)) / samples_per_cell;
            const double ratio = cells / kSurfaceFilterSigma;
            weights[tap] = std::exp(-0.5 * ratio * ratio);
        }

        return weights;
    }

    // One axis of the pair. Offsets outside the field are air, never a replicated
    // edge texel. The centre keeps its own label; an empty centre adopts the
    // nearest nonzero tap label (ties: smaller label, then the smaller signed
    // offset), which is the deterministic halo rule both passes share.
    [[nodiscard]] std::vector<SurfaceTexel> surfaceFilterAxis(
        const std::vector<SurfaceTexel> &field, const u32 raster, const double samples_per_cell, const u32 halo,
        const bool horizontal) {
        const std::vector<double> weights = surfaceFilterWeights(samples_per_cell, halo);
        const auto at = [&](const int x, const int y) -> const SurfaceTexel * {
            if (x < 0 or y < 0 or x >= int(raster) or y >= int(raster)) {
                return nullptr;
            }

            return &field[std::size_t(y) * raster + std::size_t(x)];
        };

        std::vector<SurfaceTexel> out(field.size());
        for (u32 y = 0u; y < raster; ++y) {
            for (u32 x = 0u; x < raster; ++x) {
                const auto tap_at = [&](const int offset) {
                    return horizontal ? at(int(x) + offset, int(y)) : at(int(x), int(y) + offset);
                };
                u32 mask = at(int(x), int(y))->m_label;
                if (mask == 0u) {
                    for (u32 distance = 1u; distance <= halo; ++distance) {
                        const SurfaceTexel *const negative = tap_at(-int(distance));
                        const SurfaceTexel *const positive = tap_at(int(distance));
                        const u32 negative_label = negative != nullptr ? negative->m_label : 0u;
                        const u32 positive_label = positive != nullptr ? positive->m_label : 0u;
                        if (negative_label == 0u and positive_label == 0u)
                            continue;
                        if (negative_label == 0u) {
                            mask = positive_label;
                            break;
                        }
                        if (positive_label == 0u) {
                            mask = negative_label;
                            break;
                        }

                        mask = negative_label <= positive_label ? negative_label : positive_label;
                        break;
                    }
                }

                SurfaceTexel &result = out[std::size_t(y) * raster + x];
                if (mask == 0u)
                    continue;

                double denominator = 0.0;
                std::array<double, 4u> sum{};
                for (u32 tap = 0u; tap < weights.size(); ++tap) {
                    const SurfaceTexel *const neighbour = tap_at(int(tap) - int(halo));
                    const u32 label = neighbour != nullptr ? neighbour->m_label : 0u;
                    if (label != 0u and label != mask)
                        continue;

                    denominator += weights[tap];
                    if (label == mask) {
                        sum[0u] += weights[tap] * neighbour->m_base.x;
                        sum[1u] += weights[tap] * neighbour->m_base.y;
                        sum[2u] += weights[tap] * neighbour->m_base.z;
                        sum[3u] += weights[tap] * neighbour->m_density;
                    }
                }

                const double inverse = denominator > 0.0 ? 1.0 / denominator : 0.0;
                result.m_label = mask;
                result.m_base = Double3{sum[0u] * inverse, sum[1u] * inverse, sum[2u] * inverse};
                result.m_density = sum[3u] * inverse;
            }
        }

        return out;
    }

    // Both passes in the shipped order: horizontal, then vertical over the
    // horizontal result INCLUDING its extended labels.
    [[nodiscard]] std::vector<SurfaceTexel> surfaceFilterField(
        const std::vector<SurfaceTexel> &field, const u32 raster, const double samples_per_cell, const u32 halo) {
        return surfaceFilterAxis(
            surfaceFilterAxis(field, raster, samples_per_cell, halo, true), raster, samples_per_cell, halo, false);
    }

    // The half-plane neutral rest lattice of the design's geometric model:
    // particles at (i*s, -j*s) for integer i and j >= 0, so the row at y = 0 is the
    // free surface. Every particle keeps the retained profile and reference area, so
    // a wider spacing IS the design's "+10 % dilation": the same particles occupy
    // 1.21 times the area and the bulk mean falls to V/s^2 = .82645 at s = .55.
    [[nodiscard]] double neutralLatticeDensity(
        const double x, const double y, const double spacing, const double radius) noexcept {
        const DensityKernel neutral = densityKernel({1.0, 0.0}, 0.0, 0.0);
        const double support = kDensitySurfaceRadiusScale * radius;
        double density = 0.0;
        for (int row = 0; row < 3; ++row) {
            const int centre = static_cast<int>(std::floor(x / spacing));
            for (int column = centre - 3; column <= centre + 3; ++column) {
                density += densityWeight(
                    {(x - double(column) * spacing) / support, (y + double(row) * spacing) / support}, neutral);
            }
        }

        return density;
    }

    // The same lattice after the documented separable filter, with this frame's own
    // tap count: out-of-support taps carry zero density and still contribute their
    // weight, exactly like air.
    [[nodiscard]] double filteredLatticeDensity(
        const double x, const double y, const double spacing, const double radius, const double samples_per_cell,
        const u32 halo) noexcept {
        const std::vector<double> weights = surfaceFilterWeights(samples_per_cell, halo);
        double numerator = 0.0;
        double denominator = 0.0;
        for (u32 row = 0u; row < weights.size(); ++row) {
            for (u32 column = 0u; column < weights.size(); ++column) {
                const double weight = weights[row] * weights[column];
                denominator += weight;
                numerator += weight * neutralLatticeDensity(
                                          x + (double(column) - double(halo)) / samples_per_cell,
                                          y - (double(row) - double(halo)) / samples_per_cell, spacing, radius);
            }
        }

        return denominator > 0.0 ? numerator / denominator : 0.0;
    }

    // Peak-to-valley height of the outer D = theta contour over one lattice period,
    // sampled 256 times, which is the discrete-shift reference the design states its
    // .006/.016-cell targets in (revision 7 "7-E", 1080/span64 row).
    [[nodiscard]] double latticeContourAmplitude(
        const double spacing, const double samples_per_cell, const u32 halo, const double radius) {
        constexpr u32 kSamples = 256u;
        double minimum = 1.0e30;
        double maximum = -1.0e30;
        for (u32 sample = 0u; sample < kSamples; ++sample) {
            const double x = spacing * double(sample) / double(kSamples);
            // The unfiltered contour sits at .379 cell and the filter can only move
            // it outward, so [.25, 1] brackets every case measured here.
            double low = 0.25;
            double high = 1.0;
            for (u32 step = 0u; step < 32u; ++step) {
                const double middle = 0.5 * (low + high);
                if (filteredLatticeDensity(x, middle, spacing, radius, samples_per_cell, halo) > kDensityThreshold) {
                    low = middle;
                } else {
                    high = middle;
                }
            }

            const double height = 0.5 * (low + high);
            minimum = (std::min) (minimum, height);
            maximum = (std::max) (maximum, height);
        }

        return maximum - minimum;
    }

    PPR_UNIT_TEST (particle_flow_cue_stretches_the_major_axis_only) {
        // Appearance probe for the retained flow cue: the drawn shape stretches
        // along the stored orientation axis by at most k = 1.5, the minor semi-axis
        // stays the sealed support R, and the 3.0625 r cull margin covers both.
        // The per-cell sampler below now feeds the prepare pass's one target per
        // accepted publication; the draw decodes the stored axis (see the
        // particle_orientation_* cases).
        const float support = kGridParticleSupportScale;
        const float cap = kGridParticleStretchCap;

        // Magnitude map: exactly 1 at rest, exactly the cap at and beyond the
        // reference magnitude, monotone and bounded over the admissible range.
        PPR_TEST_ASSERT(gridParticleFlowStretch(0.0f) == 1.0f);
        PPR_TEST_ASSERT(std::bit_cast<u32>(gridParticleFlowStretch(0.0f)) == std::bit_cast<u32>(1.0f));
        PPR_TEST_ASSERT(gridParticleFlowStretch(kGridParticleMotionReference) == cap);
        PPR_TEST_ASSERT(gridParticleFlowStretch(4.0f * kGridParticleMotionReference) == cap);
        PPR_TEST_ASSERT(gridParticleFlowStretch(64.0f) == cap);
        float previous = gridParticleFlowStretch(0.0f);
        for (u32 step = 1u; step <= 64u; ++step) {
            const float magnitude = static_cast<float>(step) * (kGridParticleMotionReference / 64.0f);
            const float stretch = gridParticleFlowStretch(magnitude);
            PPR_TEST_ASSERT(stretch >= previous and stretch >= 1.0f and stretch <= cap);
            previous = stretch;
        }

        // Support: the major semi-axis is k R = 3.000 r and the minor semi-axis is
        // R = 2.000 r, so max(major, minor) stays inside the 3.0625 r cull margin
        // that the prepare `visible()` test applies. Zero motion (k = 1) keeps the
        // un-stretched support.
        const float major = support * cap;
        const float minor = support;
        PPR_TEST_ASSERT(std::abs(major - 3.0f) <= 1.0e-6f and minor == 2.0f);
        PPR_TEST_ASSERT((std::max) (major, minor) <= kGridParticleCullRadiusScale);
        PPR_TEST_ASSERT(gridParticleFlowStretch(0.0f) * support == support);

        // Billboards and the stretch: the quad half-extent is R * k, and every
        // point of the stretched shape lands inside it, so the shape is clipped by
        // neither its own quad nor the frustum.
        constexpr u32 kDirections = 16u;
        constexpr u32 kBoundarySamples = 8u;
        for (u32 direction = 0u; direction < kDirections; ++direction) {
            const FlowPoint flow = gridParticleDirection(2.0 * kPi * double(direction) / double(kDirections));

            // The flow-aligned boundary point reaches exactly k R.
            const FlowPoint tip = gridParticleFlowWorld({flow.x * double(minor), flow.y * double(minor)}, flow, cap);
            PPR_TEST_ASSERT(std::abs(gridParticleLength(tip) - double(major)) <= 1.0e-6);
            // The cross-flow boundary point does not move at all: the minor axis
            // stays R for every direction, so the diagonal seam stays closed.
            const FlowPoint side = gridParticleFlowWorld({-flow.y * double(minor), flow.x * double(minor)}, flow, cap);
            PPR_TEST_ASSERT(std::abs(side.x + flow.y * double(minor)) <= 1.0e-12);
            PPR_TEST_ASSERT(std::abs(side.y - flow.x * double(minor)) <= 1.0e-12);

            for (u32 sample = 0u; sample < kBoundarySamples; ++sample) {
                const FlowPoint base = gridParticleDirection(2.0 * kPi * double(sample) / double(kBoundarySamples));
                const FlowPoint world = gridParticleFlowWorld(
                    {base.x * double(minor), base.y * double(minor)}, flow, cap);
                PPR_TEST_ASSERT(gridParticleLength(world) <= double(major) + 1.0e-6);
            }
        }

        // Containment makes the seeded interior of plan §5.2 direction-free: the
        // stretched shape contains the un-stretched one, so a lattice midpoint
        // (0.25 cell), a diagonal midpoint and a cell centre (0.3536 cell) keep
        // alpha = 1 for every flow direction and every offset direction.
        constexpr double r_cell = 0.28209479177387814; // sqrt(0.25/pi) for volume 0.25
        constexpr std::array<double, 2u> distances{0.25, 0.35355339059327373};
        const double inner = double(kGridParticleFalloffInnerFraction);
        for (u32 direction = 0u; direction < kDirections; ++direction) {
            const FlowPoint flow = gridParticleDirection(2.0 * kPi * double(direction) / double(kDirections));
            for (u32 offset = 0u; offset < kDirections; ++offset) {
                const FlowPoint axis = gridParticleDirection(2.0 * kPi * double(offset) / double(kDirections));
                for (const double distance: distances) {
                    const FlowPoint local = gridParticleFlowLocal(
                        {axis.x * distance / r_cell, axis.y * distance / r_cell}, flow, cap);
                    const double rho = gridParticleRho(local);
                    const double isotropic = distance / r_cell / double(support);

                    PPR_TEST_ASSERT(rho <= inner);
                    PPR_TEST_ASSERT(rho <= isotropic + 1.0e-12);
                    PPR_TEST_ASSERT(gridParticleFalloff(static_cast<float>(rho)) == 1.0f);
                }
            }
        }

        // Deterministic: the same row and direction produce the same shape.
        const FlowPoint sample_local{0.37, -1.21};
        const FlowPoint sample_flow = gridParticleDirection(0.7);
        const FlowPoint first = gridParticleFlowLocal(sample_local, sample_flow, 1.17f);
        const FlowPoint second = gridParticleFlowLocal(sample_local, sample_flow, 1.17f);
        PPR_TEST_ASSERT(first.x == second.x and first.y == second.y);
        PPR_TEST_ASSERT(first.x != sample_local.x or first.y != sample_local.y);

        // No invariant moved beyond the bounded constant change: the shared
        // support, the raised cull, the cap and the falloff/coverage floor
        // constants are the ones the sealing proof, the phase shapes, the
        // occlusion decision and the material/transport contracts use.
        PPR_TEST_ASSERT(kGridParticleSupportScale == 2.0f);
        PPR_TEST_ASSERT(kGridParticleCullRadiusScale == 3.0625f);
        PPR_TEST_ASSERT(kGridParticleStretchCap == 1.5f);
        PPR_TEST_ASSERT(kGridParticleMotionReference == 0.06f);
        PPR_TEST_ASSERT(kGridParticleFalloffInnerFraction == 2.0f / 3.0f);
        PPR_TEST_ASSERT(kGridParticleCoverageFloor == 1.0f / 255.0f);

        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL particle_flow major={:.4f} minor={:.4f} cull={:.4f} "
                "reference={:.3f} stretch@reference={:.4f}",
                major, minor, kGridParticleCullRadiusScale, kGridParticleMotionReference,
                gridParticleFlowStretch(kGridParticleMotionReference)));
    };

    PPR_UNIT_TEST (particle_flow_cue_is_the_identity_at_zero_motion) {
        // The seed and paused cases: a row whose published motion is zero draws
        // today's un-stretched disk/rounded square, bit for bit, so a world that
        // has not stepped and a paused world look exactly as they do today.
        PPR_TEST_ASSERT(gridParticleFlowStretch(0.0f) == 1.0f);

        constexpr u32 kDirections = 8u;
        constexpr std::array<FlowPoint, 4u> locals{{{0.0, 0.0}, {2.0, 0.0}, {-1.25, 0.75}, {0.3, -1.9}}};
        for (u32 direction = 0u; direction < kDirections; ++direction) {
            const FlowPoint flow = gridParticleDirection(2.0 * kPi * double(direction) / double(kDirections));
            for (const FlowPoint local: locals) {
                const FlowPoint identity = gridParticleFlowLocal(local, flow, gridParticleFlowStretch(0.0f));
                PPR_TEST_ASSERT(identity.x == local.x and identity.y == local.y);
            }
        }

        // The vertex extent at k = 1 is the unchanged binary32 expression, so the
        // billboard itself is bit-identical and no direction is consulted.
        const float radius = 0.2821f;
        const float extent = radius * (kGridParticleSupportScale * gridParticleFlowStretch(0.0f));
        PPR_TEST_ASSERT(std::bit_cast<u32>(extent) == std::bit_cast<u32>(radius * kGridParticleSupportScale));

        // The cue reads the existing motion pair of the 24-byte row and adds no
        // storage, so the packet, solver and export layout are untouched.
        PPR_TEST_ASSERT(sizeof(ParticleGpu) == 24u);
        PPR_TEST_ASSERT(PPR_OFFSETOF(ParticleGpu, m_motion) == 20u);

        // No pop when a world starts stepping: the elongation picks up
        // continuously, one saturating ratio step at a time.
        const float increment = gridParticleFlowStretch(kGridParticleMotionReference / 64.0f) - 1.0f;
        PPR_TEST_ASSERT(increment > 0.0f);
        PPR_TEST_ASSERT(increment <= (kGridParticleStretchCap - 1.0f) / 63.0f);

        // The sampled field is zero-motion clean at both ends: a field whose
        // winners all carry zero motion samples to exactly (0,0), not to a
        // denormal, so the fragment takes the k == 1 path bit for bit; and the
        // per-row clamp (-64..64 cells/step, so at most sqrt(2)*64 on the
        // diagonal of the bilinear average) still saturates the same cap.
        const auto flat_zero = [](const int, const int, FlowPoint &motion) {
            motion = {0.0, 0.0};
            return true;
        };
        const auto empty = [](const int, const int, FlowPoint &motion) {
            motion = {};
            return false;
        };
        const FlowPoint zero_sample = gridParticleFlowMotion(flat_zero, FlowPoint{4.3, 5.7}, FlowPoint{0.25, -0.5});
        PPR_TEST_ASSERT(zero_sample.x == 0.0 and zero_sample.y == 0.0);
        PPR_TEST_ASSERT(std::bit_cast<u32>(gridParticleSampleStretch(zero_sample)) == std::bit_cast<u32>(1.0f));
        PPR_TEST_ASSERT(gridParticleFlowLocal(FlowPoint{0.37, -1.21}, FlowPoint{1.0, 0.0}, 1.0f).x == 0.37);
        PPR_TEST_ASSERT(gridParticleFlowStretch(std::sqrt(2.0f) * 64.0f) == kGridParticleStretchCap);
        // An entirely empty field falls back to the row's own motion exactly.
        const FlowPoint fallback = gridParticleFlowMotion(empty, FlowPoint{4.3, 5.7}, FlowPoint{0.0, 0.0});
        PPR_TEST_ASSERT(fallback.x == 0.0 and fallback.y == 0.0);
        const FlowPoint fallback_moving = gridParticleFlowMotion(empty, FlowPoint{4.3, 5.7}, FlowPoint{0.021, -0.004});
        PPR_TEST_ASSERT(
            std::bit_cast<u64>(fallback_moving.x) == std::bit_cast<u64>(0.021) and
            std::bit_cast<u64>(fallback_moving.y) == std::bit_cast<u64>(-0.004));

        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL particle_flow_zero extent={:.6f} unchanged={:.6f} increment={:.6f}", extent,
                radius * kGridParticleSupportScale, increment));
    };

    PPR_UNIT_TEST (particle_flow_cue_samples_a_per_cell_motion_field) {
        // Continuity probe for the Task 4 fix: the cue no longer reads one row's own
        // published motion but a per-cell winner-motion field sampled bilinearly at
        // the particle centre, so the stretch and the flow direction are C0 in
        // position - the owner's report was that the cue "abruptly changes when moving
        // from one cell to another". The probe is split by coordinate: the sampler's
        // four taps change at the cell centres (k + 0.5), which is where the bilinear
        // window shifts and where the old per-cell cue stepped, while an integer
        // boundary is where a winner swap or a fill update lands, which is a bounded
        // field update rather than a window shift.
        //
        // Synthetic field: a gentle linear shear, about 0.05 cell/step in magnitude
        // with a 0.003 cell/step-per-cell gradient, so the cue stays well inside its
        // saturating ratio and the asserted bounds measure the sampler, not the cap.
        // Out-of-range is empty, exactly like the shader's kResolvedEdge guard.
        const auto field = [](const int x, const int y, FlowPoint &motion) {
            if (x < 0 or y < 0 or x >= 64 or y >= 64)
                return false;

            motion = {
                0.040 + 0.003 * double(x - 4) - 0.001 * double(y - 4),
                0.030 + 0.002 * double(y - 4) + 0.001 * double(x - 4)};
            return true;
        };
        const FlowPoint row_motion{0.0, 0.0};
        constexpr double kStep = 0.01;
        constexpr double kContinuityBound = 1.0e-3;

        // The two samplers under test read the same field: the shipped bilinear
        // reconstruction and the box control that holds the window's base cell. A
        // bound only one of them meets bounds the interpolation, not the field.
        const auto bilinear = [&](const FlowPoint position) {
            return gridParticleFlowMotion(field, position, row_motion);
        };
        const auto box = [&](const FlowPoint position) {
            return gridParticleFlowMotionBox(field, position, row_motion);
        };
        const auto measure = [](const auto &sampler, const std::span<const std::pair<FlowPoint, FlowPoint>> pairs) {
            ContinuityProbe probe{};
            for (const auto &[here, there]: pairs) {
                const FlowPoint first = sampler(here);
                const FlowPoint second = sampler(there);
                probe.m_stretch = (std::max) (probe.m_stretch,
                    std::abs(double(gridParticleSampleStretch(second)) - double(gridParticleSampleStretch(first))));
                probe.m_angle = (std::max) (probe.m_angle,
                    gridParticleAngleDifference(gridParticleFlowAngle(second), gridParticleFlowAngle(first)));
            }

            return probe;
        };

        // Sub-cell sweep, fixed field: 0.01-cell pairs over every offset of four
        // windows, so the sweep contains the pair that steps over a window edge into
        // the next one.
        std::vector<std::pair<FlowPoint, FlowPoint>> subcell_pairs;
        for (u32 window = 0u; window < 4u; ++window) {
            for (u32 offset = 0u; offset < 100u; ++offset) {
                const double x = 2.5 + double(window) + double(offset) * kStep;
                const double y = 2.5 + double(window) + double(offset) * kStep;
                subcell_pairs.emplace_back(FlowPoint{x, y}, FlowPoint{x + kStep, y});
                subcell_pairs.emplace_back(FlowPoint{x, y}, FlowPoint{x, y + kStep});
            }
        }

        // Sampler-boundary sweep, fixed field: the pair straddling a window edge at
        // cell_position = k + 0.5 +/- 0.005. The four taps change there - the field is
        // published at the cell centres, not at the integer corners - and that is
        // where the per-cell cue stepped. The entering tap's weight vanishes exactly
        // at the edge, so the bilinear sample is C0 there.
        std::vector<std::pair<FlowPoint, FlowPoint>> boundary_pairs;
        for (int edge = 2; edge <= 7; ++edge) {
            for (u32 offset = 0u; offset <= 40u; ++offset) {
                const double position = 2.0 + 0.1 * double(offset);
                const double centre = double(edge) + 0.5;
                boundary_pairs.emplace_back(FlowPoint{centre - 0.005, position}, FlowPoint{centre + 0.005, position});
                boundary_pairs.emplace_back(FlowPoint{position, centre - 0.005}, FlowPoint{position, centre + 0.005});
            }
        }

        // Integer-coordinate sweep, fixed field: base = floor(cell_position - 0.5) is
        // unmoved across an integer, so neither sampler steps there and this sweep
        // cannot discriminate - both are asserted continuous on it. It is kept as the
        // field-change case: a winner swap or a fill update is what moves the sample
        // at an integer, and the update probe below pins that step to the tap weight.
        std::vector<std::pair<FlowPoint, FlowPoint>> integer_pairs;
        for (int edge = 2; edge <= 7; ++edge) {
            for (u32 offset = 0u; offset <= 40u; ++offset) {
                const double position = 2.0 + 0.1 * double(offset);
                const double corner = double(edge);
                integer_pairs.emplace_back(FlowPoint{corner - 0.005, position}, FlowPoint{corner + 0.005, position});
                integer_pairs.emplace_back(FlowPoint{position, corner - 0.005}, FlowPoint{position, corner + 0.005});
            }
        }

        const ContinuityProbe subcell = measure(bilinear, subcell_pairs);
        const ContinuityProbe boundary = measure(bilinear, boundary_pairs);
        const ContinuityProbe integer = measure(bilinear, integer_pairs);
        const ContinuityProbe box_subcell = measure(box, subcell_pairs);
        const ContinuityProbe box_boundary = measure(box, boundary_pairs);
        const ContinuityProbe box_integer = measure(box, integer_pairs);

        // The shipped sampler passes both continuity sweeps. Measured on this grid:
        // 2.5e-4 of stretch and 5.1e-4 rad of direction on the sub-cell sweep,
        // 2.6e-4 and 5.3e-4 across the window edge, i.e. an order of magnitude inside
        // the bound.
        PPR_TEST_ASSERT(subcell.m_stretch <= kContinuityBound);
        PPR_TEST_ASSERT(subcell.m_angle <= kContinuityBound);
        PPR_TEST_ASSERT(boundary.m_stretch <= kContinuityBound);
        PPR_TEST_ASSERT(boundary.m_angle <= kContinuityBound);

        // The box control fails them, which is what makes them evidence: its step at a
        // window edge is the whole difference between two cells (this field's 0.003
        // cell/step-per-cell gradient), so it cannot pass the sub-cell sweep (which
        // contains the crossing pair) or the boundary sweep. A centre-nearest box
        // would step at integer coordinates instead; that is why the window hold, whose
        // steps land where the four taps change, is the control for these bounds.
        PPR_TEST_ASSERT(box_subcell.m_stretch > kContinuityBound);
        PPR_TEST_ASSERT(box_subcell.m_angle > kContinuityBound);
        PPR_TEST_ASSERT(box_boundary.m_stretch > kContinuityBound);
        PPR_TEST_ASSERT(box_boundary.m_angle > kContinuityBound);

        // The integer sweep is where the box is as continuous as the shipped sampler:
        // an earlier revision straddled integers, so a box and the bilinear sampler
        // both passed it and the probe proved nothing about the sampler boundary.
        PPR_TEST_ASSERT(integer.m_stretch <= kContinuityBound);
        PPR_TEST_ASSERT(integer.m_angle <= kContinuityBound);
        PPR_TEST_ASSERT(box_integer.m_stretch <= kContinuityBound);
        PPR_TEST_ASSERT(box_integer.m_angle <= kContinuityBound);

        // Field-update bound: changing one lattice cell's motion changes the sample
        // by exactly that tap's bilinear weight times the delta - not by a fudged
        // constant. At p = (4.7, 4.3) the base cell is (4, 3) with fraction
        // (0.2, 0.8), so cell (4, 4) is the (0, 1) tap with weight (1-fx)*fy = 0.64.
        const auto shifted = [&](const int x, const int y, FlowPoint &motion) {
            if (not field(x, y, motion))
                return false;

            if (x == 4 and y == 4)
                motion = {motion.x + 0.002, motion.y + 0.005};
            return true;
        };
        const FlowPoint update_point{4.7, 4.3};
        const FlowPoint before_update = gridParticleFlowMotion(field, update_point, row_motion);
        const FlowPoint after_update = gridParticleFlowMotion(shifted, update_point, row_motion);
        const double tap_weight = (1.0 - 0.2) * 0.8;
        PPR_TEST_ASSERT(std::abs(after_update.x - before_update.x - tap_weight * 0.002) <= 1.0e-6);
        PPR_TEST_ASSERT(std::abs(after_update.y - before_update.y - tap_weight * 0.005) <= 1.0e-6);
        // The documented bound: the step is that one lattice value's contribution,
        // so the stretch moves by at most the saturating slope times it. This delta
        // measures 0.02460, inside the plan's 0.05 figure; at a lattice point the
        // same tap has weight 1 and the bound is the full delta instead.
        const double update_stretch = std::abs(
            double(gridParticleSampleStretch(after_update)) - double(gridParticleSampleStretch(before_update)));
        PPR_TEST_ASSERT(std::abs(update_stretch - 0.024603) <= 1.0e-5);
        PPR_TEST_ASSERT(update_stretch <= 0.05);

        // The same change across an integer coordinate, which is the case the integer
        // sweep above is kept for: base = floor(cell_position - 0.5) is unmoved across
        // x = 4, so the step there is the field change alone, weighted by the updating
        // tap at each side - 0.2475 below the integer, 0.2525 above.
        for (const double x: {4.0 - 0.005, 4.0 + 0.005}) {
            const double fraction = (x - 0.5) - std::floor(x - 0.5);
            const double weight_at = fraction * 0.5;
            const FlowPoint before = gridParticleFlowMotion(field, FlowPoint{x, 4.0}, row_motion);
            const FlowPoint after = gridParticleFlowMotion(shifted, FlowPoint{x, 4.0}, row_motion);
            PPR_TEST_ASSERT(std::abs(after.x - before.x - weight_at * 0.002) <= 1.0e-12);
            PPR_TEST_ASSERT(std::abs(after.y - before.y - weight_at * 0.005) <= 1.0e-12);
        }

        // A winner swap is the other half of the field update: the value changes by
        // the whole difference, still weighted by the same tap. Asserted at the tap's
        // full weight, which is the dual-lattice point cell_position = integer + 0.5:
        // there the sample is exactly that one lattice value, so swapping it for zero
        // motion moves the sample by the whole value.
        const auto swapped = [&](const int x, const int y, FlowPoint &motion) {
            if (not field(x, y, motion))
                return false;

            if (x == 4 and y == 4)
                motion = {0.0, 0.0};
            return true;
        };
        const FlowPoint lattice_point{4.5, 4.5};
        const FlowPoint before_swap = gridParticleFlowMotion(field, lattice_point, row_motion);
        const FlowPoint after_swap = gridParticleFlowMotion(swapped, lattice_point, row_motion);
        PPR_TEST_ASSERT(after_swap.x == 0.0 and after_swap.y == 0.0);
        // The sample there is exactly the lattice value the field carries, so the
        // swap moves it by exactly (0.040, 0.030) - the full-word winner-swap bound.
        PPR_TEST_ASSERT(std::abs(before_swap.x - after_swap.x - 0.040) <= 1.0e-12);
        PPR_TEST_ASSERT(std::abs(before_swap.y - after_swap.y - 0.030) <= 1.0e-12);

        // Deterministic and order-independent: the same field, position and row
        // motion always produce the same sample bit for bit.
        const FlowPoint first = gridParticleFlowMotion(field, FlowPoint{4.37, 5.61}, row_motion);
        const FlowPoint second = gridParticleFlowMotion(field, FlowPoint{4.37, 5.61}, row_motion);
        PPR_TEST_ASSERT(
            std::bit_cast<u64>(first.x) == std::bit_cast<u64>(second.x) and
            std::bit_cast<u64>(first.y) == std::bit_cast<u64>(second.y));
        // ... and the field really is filtered rather than dropped: the sample differs
        // from the row's own published motion wherever the neighbours do.
        PPR_TEST_ASSERT(first.x != row_motion.x or first.y != row_motion.y);

        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL particle_flow_sampler subcell_stretch={:.6f} subcell_angle={:.6f} "
                "cross_stretch={:.6f} cross_angle={:.6f} update_stretch={:.6f} tap_weight={:.2f} "
                "box_subcell={:.6f} box_cross={:.6f} box_integer={:.6f}",
                subcell.m_stretch, subcell.m_angle, boundary.m_stretch, boundary.m_angle, update_stretch, tap_weight,
                box_subcell.m_stretch, box_boundary.m_stretch, box_integer.m_stretch));
    };

    // ------------------------------------------------------------------
    // free-surface renormalisation: partial tap sets
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (particle_flow_cue_renormalises_partial_taps) {
        // particleFlowMotion renormalises by the sum of the weights that are
        // present above W = 1/4, and that branch is what the free surface depends
        // on: a particle centre inside a cell with one, two or three committed
        // neighbours samples the winners that exist rather than an average diluted
        // to a quarter or a half by the empty cells. Below W = 1/4 the sample
        // blends continuously toward the row's own motion, so the W -> 0 limit no
        // longer depends on the approach direction. The probes pin both branches,
        // show the divisor is weight-invariant above the gate, and bound the step
        // when a non-uniform neighbour disappears.
        constexpr FlowPoint kSample{4.7, 4.3}; // base (4, 3), fraction (0.2, 0.8)
        const std::array<std::array<int, 2u>, 4u> cells{{{4, 3}, {5, 3}, {4, 4}, {5, 4}}};
        const std::array<FlowPoint, 4u> values{{{0.040, 0.030}, {0.050, 0.040}, {0.030, 0.020}, {0.060, 0.050}}};

        // The premise the arithmetic below rests on: all four taps have a non-zero
        // weight at this sample, so every subset of them is a distinct partial set.
        const FlowPoint p{kSample.x - 0.5, kSample.y - 0.5};
        const double base_x = std::floor(p.x);
        const double base_y = std::floor(p.y);
        const double fraction_x = p.x - base_x;
        const double fraction_y = p.y - base_y;
        const std::array<double, 4u> weights{
            (1.0 - fraction_x) * (1.0 - fraction_y), fraction_x * (1.0 - fraction_y), (1.0 - fraction_x) * fraction_y,
            fraction_x * fraction_y};
        PPR_TEST_ASSERT(base_x == 4.0 and base_y == 3.0);
        PPR_TEST_ASSERT(std::abs(weights[0] - 0.16) <= 1.0e-12);
        PPR_TEST_ASSERT(std::abs(weights[1] - 0.04) <= 1.0e-12);
        PPR_TEST_ASSERT(std::abs(weights[2] - 0.64) <= 1.0e-12);
        PPR_TEST_ASSERT(std::abs(weights[3] - 0.16) <= 1.0e-12);
        PPR_TEST_ASSERT(std::abs(weights[0] + weights[1] + weights[2] + weights[3] - 1.0) <= 1.0e-12);

        // A field that commits exactly the taps in `mask`, each with its own motion.
        // Every other cell is empty, which is the free surface the shader's guard
        // rejects.
        const auto masked = [&cells, &values](const u32 mask) {
            return [&cells, &values, mask](const int x, const int y, FlowPoint &motion) {
                for (u32 tap = 0u; tap < cells.size(); ++tap) {
                    if ((mask & (1u << tap)) == 0u or cells[tap][0] != x or cells[tap][1] != y)
                        continue;

                    motion = values[tap];
                    return true;
                }

                return false;
            };
        };

        // Every non-empty subset follows the shipped continuous extension: above
        // W = 1/4 it is the weighted mean of the taps that exist; below W = 1/4 it
        // is the blend toward the row's own motion. The masks cover the four single
        // taps, the six pairs and the four triples: 1, 2 and 3 present taps at the
        // same sample.
        u32 partial_configurations = 0u;
        for (u32 mask = 1u; mask < 0x0fu; ++mask) {
            double sum_x = 0.0;
            double sum_y = 0.0;
            double weight = 0.0;
            for (u32 tap = 0u; tap < cells.size(); ++tap) {
                if ((mask & (1u << tap)) == 0u)
                    continue;

                sum_x += weights[tap] * values[tap].x;
                sum_y += weights[tap] * values[tap].y;
                weight += weights[tap];
            }

            const FlowPoint sample = gridParticleFlowMotion(masked(mask), kSample, FlowPoint{0.0, 0.0});
            const double expected_x = weight < 0.25 ? 4.0 * sum_x : sum_x / weight;
            const double expected_y = weight < 0.25 ? 4.0 * sum_y : sum_y / weight;
            PPR_TEST_ASSERT(std::abs(sample.x - expected_x) <= 1.0e-12);
            PPR_TEST_ASSERT(std::abs(sample.y - expected_y) <= 1.0e-12);
            ++partial_configurations;
        }
        PPR_TEST_ASSERT(partial_configurations == 14u);

        // The four single-tap masks straddle the gate. A lone tap with weight
        // >= 1/4 is returned at full magnitude (the 0.64 tap); the lighter taps
        // (0.16, 0.04, 0.16) are below W = 1/4 and contribute 4W of their value
        // with zero row motion, so a sliver of coverage no longer asserts a
        // trusted direction.
        for (u32 tap = 0u; tap < cells.size(); ++tap) {
            const FlowPoint sample = gridParticleFlowMotion(masked(1u << tap), kSample, FlowPoint{0.0, 0.0});
            const double expected_scale = weights[tap] < 0.25 ? 4.0 * weights[tap] : 1.0;
            PPR_TEST_ASSERT(std::abs(sample.x - expected_scale * values[tap].x) <= 1.0e-12);
            PPR_TEST_ASSERT(std::abs(sample.y - expected_scale * values[tap].y) <= 1.0e-12);
        }

        // Weight-invariance for a uniform field above the gate: with every present
        // winner carrying the same motion, a tap turning absent does not move the
        // sample, because the divisor shrinks by exactly that tap's weight. A
        // uniform interior therefore does not fade as its free-surface boundary
        // loses taps. Subsets below W = 1/4 instead fade by exactly 4W toward a
        // zero row motion, which is the documented domain-hardening branch.
        constexpr FlowPoint kUniform{0.041, -0.027};
        const auto uniform_field = [&cells, kUniform](const u32 mask) {
            return [&cells, kUniform, mask](const int x, const int y, FlowPoint &motion) {
                for (u32 tap = 0u; tap < cells.size(); ++tap) {
                    if ((mask & (1u << tap)) == 0u or cells[tap][0] != x or cells[tap][1] != y)
                        continue;

                    motion = kUniform;
                    return true;
                }

                return false;
            };
        };
        for (u32 mask = 1u; mask < 0x0fu; ++mask) {
            double weight = 0.0;
            for (u32 tap = 0u; tap < cells.size(); ++tap) {
                if ((mask & (1u << tap)) != 0u)
                    weight += weights[tap];
            }

            const FlowPoint sample = gridParticleFlowMotion(uniform_field(mask), kSample, FlowPoint{0.0, 0.0});
            const double expected_scale = weight < 0.25 ? 4.0 * weight : 1.0;
            PPR_TEST_ASSERT(std::abs(sample.x - expected_scale * kUniform.x) <= 1.0e-12);
            PPR_TEST_ASSERT(std::abs(sample.y - expected_scale * kUniform.y) <= 1.0e-12);
        }

        // The W -> 0 limit is now approach-independent: an empty window returns the
        // row's own motion exactly, and one weak single tap fades continuously
        // between that and the full tap value. At W = 1/4 both branches agree.
        const FlowPoint own_motion{0.013, -0.007};
        const auto empty_field = [](const int, const int, FlowPoint &) { return false; };
        const auto lone_tap = [](const int x, const int y, FlowPoint &motion) {
            if (x != 3 or y != 3)
                return false;

            motion = FlowPoint{0.08, -0.02};
            return true;
        };
        const FlowPoint empty_sample = gridParticleFlowMotion(empty_field, FlowPoint{4.0, 4.0}, own_motion);
        PPR_TEST_ASSERT(std::bit_cast<u64>(empty_sample.x) == std::bit_cast<u64>(own_motion.x));
        PPR_TEST_ASSERT(std::bit_cast<u64>(empty_sample.y) == std::bit_cast<u64>(own_motion.y));

        // p = cell_position - 0.5; at (4.0, 4.0) the fraction is (0.5, 0.5), so the
        // single tap (3, 3) has W = 1/4 exactly. Moving y by +-0.02 makes it 0.24 or
        // 0.26, i.e. one step either side of the branch point.
        const FlowPoint at_gate = gridParticleFlowMotion(lone_tap, FlowPoint{4.0, 4.0}, own_motion);
        const FlowPoint below_gate = gridParticleFlowMotion(lone_tap, FlowPoint{4.0, 4.02}, own_motion);
        const FlowPoint above_gate = gridParticleFlowMotion(lone_tap, FlowPoint{4.0, 3.98}, own_motion);
        const FlowPoint tap_value{0.08, -0.02};
        PPR_TEST_ASSERT(std::abs(at_gate.x - tap_value.x) <= 1.0e-12);
        PPR_TEST_ASSERT(std::abs(at_gate.y - tap_value.y) <= 1.0e-12);
        PPR_TEST_ASSERT(std::abs(below_gate.x - (0.96 * tap_value.x + 0.04 * own_motion.x)) <= 1.0e-12);
        PPR_TEST_ASSERT(std::abs(below_gate.y - (0.96 * tap_value.y + 0.04 * own_motion.y)) <= 1.0e-12);
        PPR_TEST_ASSERT(std::abs(above_gate.x - tap_value.x) <= 1.0e-12);
        PPR_TEST_ASSERT(std::abs(above_gate.y - tap_value.y) <= 1.0e-12);
        PPR_TEST_ASSERT(std::abs(below_gate.x - at_gate.x) <= 0.02 and std::abs(below_gate.y - at_gate.y) <= 0.02);

        // A present tap that carries zero motion is not an absent tap: it divides into
        // the sum and contributes nothing, so it weakens the sample by its own weight.
        // On the largest tap (w = 0.64) the four-tap sample is exactly 0.36 of the
        // uniform motion, and dropping that tap restores all of it - the bounded step
        // below, instantiated at a zero value. The one uniform field where removing a
        // zero-motion tap is a no-op is the all-zero one, which every present subset
        // returns as exactly (0,0): no denormal and no 0/0 from the divisor.
        const auto zero_tap_field = [&cells, kUniform](const u32 mask) {
            return [&cells, kUniform, mask](const int x, const int y, FlowPoint &motion) {
                for (u32 tap = 0u; tap < cells.size(); ++tap) {
                    if ((mask & (1u << tap)) == 0u or cells[tap][0] != x or cells[tap][1] != y)
                        continue;

                    motion = tap == 2u ? FlowPoint{} : kUniform;
                    return true;
                }

                return false;
            };
        };
        const FlowPoint with_zero = gridParticleFlowMotion(zero_tap_field(0x0fu), kSample, FlowPoint{0.0, 0.0});
        const FlowPoint without_zero = gridParticleFlowMotion(zero_tap_field(0x0bu), kSample, FlowPoint{0.0, 0.0});
        PPR_TEST_ASSERT(std::abs(with_zero.x - (1.0 - weights[2]) * kUniform.x) <= 1.0e-12);
        PPR_TEST_ASSERT(std::abs(with_zero.y - (1.0 - weights[2]) * kUniform.y) <= 1.0e-12);
        PPR_TEST_ASSERT(std::abs(without_zero.x - kUniform.x) <= 1.0e-12);
        PPR_TEST_ASSERT(std::abs(without_zero.y - kUniform.y) <= 1.0e-12);

        const auto flat_zero_field = [&cells](const u32 mask) {
            return [&cells, mask](const int x, const int y, FlowPoint &motion) {
                for (u32 tap = 0u; tap < cells.size(); ++tap) {
                    if ((mask & (1u << tap)) == 0u or cells[tap][0] != x or cells[tap][1] != y)
                        continue;

                    motion = {};
                    return true;
                }

                return false;
            };
        };
        for (u32 mask = 1u; mask < 0x0fu; ++mask) {
            const FlowPoint sample = gridParticleFlowMotion(flat_zero_field(mask), kSample, FlowPoint{0.0, 0.0});
            PPR_TEST_ASSERT(sample.x == 0.0 and sample.y == 0.0);
        }

        // A non-uniform neighbour disappearing is the bounded step: removing the tap's
        // value from a mean whose divisor also drops by its weight moves the sample by
        //   w / (W - w) * |value - mean_with_all_taps|
        // i.e. the removed tap's weight relative to the surviving weight sum, times how
        // far its value is from the present mean. With all four present (W = 1) that
        // normalised weight is w / (1 - w): 1.7778 for the 0.64 tap, 0.1905 for the
        // 0.16 taps and 0.0417 for the 0.04 tap. Asserted exactly, not as a fudge.
        const FlowPoint full = gridParticleFlowMotion(masked(0x0fu), kSample, FlowPoint{0.0, 0.0});
        const std::array<double, 4u> normalised_weights{
            weights[0] / (1.0 - weights[0]), weights[1] / (1.0 - weights[1]), weights[2] / (1.0 - weights[2]),
            weights[3] / (1.0 - weights[3])};
        PPR_TEST_ASSERT(std::abs(normalised_weights[0] - 0.19047619) <= 1.0e-8);
        PPR_TEST_ASSERT(std::abs(normalised_weights[1] - 0.04166667) <= 1.0e-8);
        PPR_TEST_ASSERT(std::abs(normalised_weights[2] - 1.77777778) <= 1.0e-8);
        PPR_TEST_ASSERT(std::abs(normalised_weights[3] - 0.19047619) <= 1.0e-8);

        double largest_step = 0.0;
        for (u32 removed = 0u; removed < cells.size(); ++removed) {
            const FlowPoint partial = gridParticleFlowMotion(
                masked(0x0fu & ~(1u << removed)), kSample, FlowPoint{0.0, 0.0});
            const FlowPoint step{partial.x - full.x, partial.y - full.y};
            PPR_TEST_ASSERT(std::abs(step.x - normalised_weights[removed] * (full.x - values[removed].x)) <= 1.0e-12);
            PPR_TEST_ASSERT(std::abs(step.y - normalised_weights[removed] * (full.y - values[removed].y)) <= 1.0e-12);
            largest_step = (std::max) (largest_step, gridParticleLength(step));
        }
        // Measured on this probe: the worst step is the 0.64 tap's 0.0181 cell/step,
        // which is the bound a free surface can move the cue by per disappearing
        // neighbour here.
        PPR_TEST_ASSERT(largest_step <= 0.02);

        // The visible step that bound buys, on the same largest tap: the stretch map is
        // (cap - 1) / reference per cell/step, so the stretch moves by at most that
        // slope times the sample step. Measured here: 0.0181 cell/step and 0.116 of
        // stretch, and the same tap at full weight is the winner-swap case above.
        const FlowPoint largest_partial = gridParticleFlowMotion(masked(0x0bu), kSample, FlowPoint{0.0, 0.0});
        const FlowPoint largest_difference{full.x - values[2].x, full.y - values[2].y};
        const double largest_shrink = gridParticleLength(largest_difference) * normalised_weights[2];
        const double largest_stretch = std::abs(
            double(gridParticleSampleStretch(largest_partial)) - double(gridParticleSampleStretch(full)));
        PPR_TEST_ASSERT(
            std::abs(gridParticleLength({largest_partial.x - full.x, largest_partial.y - full.y}) - largest_shrink) <=
            1.0e-12);
        PPR_TEST_ASSERT(
            largest_stretch <=
            double(kGridParticleStretchCap - 1.0f) / double(kGridParticleMotionReference) * largest_shrink + 1.0e-6);

        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL particle_flow_taps partial={} weights=({:.2f},{:.2f},{:.2f},{:.2f}) "
                "full=({:.4f},{:.4f}) largest_step={:.6f} largest_stretch={:.6f} zero_tap={:.4f}",
                partial_configurations, weights[0], weights[1], weights[2], weights[3], full.x, full.y, largest_step,
                largest_stretch, with_zero.x));
    };

    PPR_UNIT_TEST (particle_tangent_contour_stays_inside_the_support) {
        // Appearance probe for the Task 11 increment 1 cue: the outer contour is
        // retargeted along a surface tangent, but only inside the band between the
        // plateau and the support, so the painted radius, the plateau, the support
        // and the cull cannot move for any orientation.
        constexpr u32 kDirections = 32u;
        constexpr double r_cell = 0.28209479177387814; // sqrt(0.25/pi) for volume 0.25
        const double support = double(kGridParticleSupportScale);
        const double inner = double(kGridParticleFalloffInnerFraction);

        // The angular edge and its legal band. k0 = 5/6 is the resting 0.5
        // contour; b = 0.3999 is the strongest four-decimal bias whose endpoints
        // stay strictly inside [2/3, 1] in binary32. The ending guard is the one
        // the shader's static_assert and the CPU twin's static_assert encode:
        // k_min > 2/3 (the falloff warp divides by that band offset) and
        // k_max <= 1 (the contour cannot pass the support radius).
        const float contour_scale = kGridParticleTangentContourScale;
        const float contour_bias = kGridParticleTangentContourBias;
        const float contour_maximum = contour_scale * (1.0f + 0.5f * contour_bias);
        const float contour_minimum = contour_scale * (1.0f - 0.5f * contour_bias);
        const float plateau = static_cast<float>(2.0 / 3.0);
        PPR_TEST_ASSERT(contour_scale == 5.0f / 6.0f);
        PPR_TEST_ASSERT(contour_bias == 0.3999f);
        PPR_TEST_ASSERT(contour_minimum > plateau);
        PPR_TEST_ASSERT(contour_maximum <= 1.0f);
        // The endpoints the plan's arithmetic pins, in float32: 0.666708350 and
        // 0.999958277. b = 0.40 would make contour_minimum == plateau exactly,
        // which is the degenerate case the constants avoid.
        PPR_TEST_ASSERT(std::abs(contour_minimum - 0.66670835f) <= 1.0e-7f);
        PPR_TEST_ASSERT(std::abs(contour_maximum - 0.99995828f) <= 1.0e-7f);
        PPR_TEST_ASSERT(contour_minimum - plateau >= 4.0e-5f);

        // Shipped (clamped) contour: the defensive clamp raises only the minor
        // endpoint, from 0.666708 to 0.666767, and does not move the major one.
        PPR_TEST_ASSERT(kGridParticleTangentContourMinimum == kGridParticleFalloffInnerFraction + 1.0e-4f);
        PPR_TEST_ASSERT(gridParticleTangentContour(0.0f) == kGridParticleTangentContourMinimum);
        PPR_TEST_ASSERT(gridParticleTangentContour(1.0f) == contour_maximum);
        PPR_TEST_ASSERT(std::abs(double(gridParticleTangentContour(0.0f)) - 0.66676670) <= 1.0e-7);
        PPR_TEST_ASSERT(std::abs(double(gridParticleTangentContour(1.0f)) - 0.99995828) <= 1.0e-7);
        // It bites only within about 0.76 degrees of the normal direction: the
        // clamp threshold solves k0*(1 + b*(cos^2 - 1/2)) == minimum.
        const double contour_step = double(kGridParticleTangentContourMinimum) - double(contour_minimum);
        const double clamp_cosine_squared = contour_step / (double(contour_scale) * double(contour_bias));
        const double clamp_angle_from_normal = std::asin(std::sqrt(clamp_cosine_squared));
        PPR_TEST_ASSERT(clamp_cosine_squared > 0.0);
        PPR_TEST_ASSERT(clamp_angle_from_normal <= 1.0 * kPi / 180.0);
        // ... and the angle is the plan's recorded 0.758 degrees.
        PPR_TEST_ASSERT(std::abs(clamp_angle_from_normal * 180.0 / kPi - 0.758) <= 0.01);

        // At span64 that is the documented 1.4997 : 1 silhouette aspect (1.49984
        // before the clamp).
        PPR_TEST_ASSERT(
            std::abs(double(gridParticleTangentContour(1.0f)) / double(gridParticleTangentContour(0.0f)) - 1.49971) <=
            1.0e-4);
        PPR_TEST_ASSERT(std::abs(double(contour_maximum) / double(contour_minimum) - 1.49984) <= 1.0e-4);
        // The support and cull constants are the retained ones, and the join
        // closure is a round-off hair rather than a shape knob.
        PPR_TEST_ASSERT(kGridParticleSupportScale == 2.0f);
        PPR_TEST_ASSERT(kGridParticleCullRadiusScale == 3.0625f);
        PPR_TEST_ASSERT(kGridParticleTangentJoinClosure > 0.0f and kGridParticleTangentJoinClosure < 1.0e-3f);

        // The angular edge is a constant of the constants, not of the orientation.
        const double major = double(gridParticleTangentContour(1.0f));
        const double minor = double(gridParticleTangentContour(0.0f));

        for (u32 index = 0u; index < kDirections; ++index) {
            const FlowPoint tangent = gridParticleDirection(2.0 * kPi * double(index) / double(kDirections));
            const FlowPoint normal{-tangent.y, tangent.x};

            // The 0.5 contour sits exactly at k(theta) along the tangent and across
            // it, so the silhouette is the angular edge and nothing else.
            const FlowPoint major_point{major * support * tangent.x, major * support * tangent.y};
            const FlowPoint minor_point{minor * support * normal.x, minor * support * normal.y};
            PPR_TEST_ASSERT(std::abs(gridParticleTangentFalloff(major_point, tangent) - 0.5) <= 1.0e-9);
            PPR_TEST_ASSERT(std::abs(gridParticleTangentFalloff(minor_point, tangent) - 0.5) <= 1.0e-9);

            // Painted radius: exactly zero at the support radius in every direction
            // and positive just inside it, so nothing is drawn beyond R. The endpoint
            // is the contour function's own contract, not only the fragment's
            // short-circuit, so it is asserted directly for this real tangent. The
            // sampled point's rounded length can land a few ulps inside R, which is
            // what the join closure covers.
            for (u32 sample = 0u; sample < kDirections; ++sample) {
                const FlowPoint direction = gridParticleDirection(2.0 * kPi * double(sample) / double(kDirections));
                const FlowPoint at_support{support * direction.x, support * direction.y};
                const FlowPoint just_inside{0.999 * support * direction.x, 0.999 * support * direction.y};

                PPR_TEST_ASSERT(gridParticleTangentFalloff(at_support, tangent) == 0.0);
                PPR_TEST_ASSERT(gridParticleShadedFalloff(at_support, tangent, true) == 0.0f);
                PPR_TEST_ASSERT(gridParticleShadedFalloff(just_inside, tangent, true) > 0.0f);
            }

            // Monotone and bounded outwards along a fixed ray.
            float previous = gridParticleShadedFalloff(FlowPoint{0.0, 0.0}, tangent, true);
            PPR_TEST_ASSERT(previous == 1.0f);
            for (u32 step = 1u; step <= 100u; ++step) {
                const double rho = double(step) / 100.0;
                const float alpha = gridParticleShadedFalloff(
                    FlowPoint{rho * support * 0.7, rho * support * 0.7}, tangent, true);
                PPR_TEST_ASSERT(alpha <= previous and alpha >= 0.0f and alpha <= 1.0f);
                previous = alpha;
            }
        }

        // The seeded interior is untouched for every orientation: the axis
        // midpoint (0.25 cell), the diagonal midpoint and the cell centre
        // (0.3536 cell) stay at alpha = 1, so no tangent can open the seam.
        constexpr std::array<double, 2u> distances{0.25, 0.35355339059327373};
        for (u32 index = 0u; index < kDirections; ++index) {
            const FlowPoint tangent = gridParticleDirection(2.0 * kPi * double(index) / double(kDirections));
            for (u32 offset = 0u; offset < kDirections; ++offset) {
                const FlowPoint axis = gridParticleDirection(2.0 * kPi * double(offset) / double(kDirections));
                for (const double distance: distances) {
                    const FlowPoint local{distance / r_cell * axis.x, distance / r_cell * axis.y};

                    PPR_TEST_ASSERT(gridParticleRho(local) <= inner);
                    PPR_TEST_ASSERT(gridParticleShadedFalloff(local, tangent, true) == 1.0f);
                }
            }
        }

        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL particle_tangent major={:.4f} minor={:.4f} inner={:.4f} cull={:.4f}",
                gridParticleTangentContour(1.0f), gridParticleTangentContour(0.0f), inner,
                kGridParticleCullRadiusScale));
    };

    PPR_UNIT_TEST (particle_tangent_follows_the_fill_field) {
        // The tangent comes from the committed per-cell Q16 fill field, read on a
        // 3x3 stencil: the four levels a boundary carries (0.25 / 0.5 / 0.75 /
        // 1.0) instead of one bit. A surface gives an outward normal (minus the
        // field gradient) and a tangent perpendicular to it; a flat field gives
        // none. The prepare pass now evaluates this once per accepted publication
        // and stores the shared axis; the draw decodes it (see the
        // particle_orientation_* cases).
        const FlowPoint centre{4.5, 5.5}; // a cell centre, so the bilinear weights collapse
        const double support = double(kGridParticleSupportScale);

        // Half-planes of owned cells at full fill. The fill rises into the body, so
        // the outward normal points away from it. A full/empty step is also the
        // field the mask-era probes used, so the pinned gradient literals below
        // still hold their meaning while the fractional-level probes follow.
        const auto body_left = [](const int x, const int) { return x <= 4 ? 1.0 : 0.0; };
        const auto body_right = [](const int x, const int) { return x >= 5 ? 1.0 : 0.0; };
        const auto body_below = [](const int, const int y) { return y <= 5 ? 1.0 : 0.0; };
        const auto body_above = [](const int, const int y) { return y >= 6 ? 1.0 : 0.0; };
        const auto body_lower_left = [](const int x, const int y) { return x + y <= 9 ? 1.0 : 0.0; };

        FlowPoint tangent{};
        double left_gradient = 0.0;
        PPR_TEST_ASSERT(gridParticleSurfaceTangent(body_left, centre, tangent, &left_gradient));
        PPR_TEST_ASSERT(std::abs(tangent.x) <= 1.0e-12 and std::abs(tangent.y - 1.0) <= 1.0e-12);
        PPR_TEST_ASSERT(std::abs(gridParticleLength(tangent) - 1.0) <= 1.0e-12);

        // Rotating the pattern by 90 degrees rotates the tangent by 90 degrees, so
        // the elongation direction follows the surface rather than the lattice.
        FlowPoint rotated{};
        PPR_TEST_ASSERT(gridParticleSurfaceTangent(body_below, centre, rotated));
        PPR_TEST_ASSERT(std::abs(rotated.x + tangent.y) <= 1.0e-12 and std::abs(rotated.y - tangent.x) <= 1.0e-12);

        PPR_TEST_ASSERT(gridParticleSurfaceTangent(body_right, centre, tangent));
        PPR_TEST_ASSERT(tangent.x == 0.0 and std::abs(tangent.y + 1.0) <= 1.0e-12);

        PPR_TEST_ASSERT(gridParticleSurfaceTangent(body_above, centre, tangent));
        PPR_TEST_ASSERT(std::abs(tangent.x - 1.0) <= 1.0e-12 and tangent.y == 0.0);

        // A diagonal surface gives a diagonal tangent, not an axis-snapped one.
        double centre_gradient = 0.0;
        PPR_TEST_ASSERT(gridParticleSurfaceTangent(body_lower_left, centre, tangent, &centre_gradient));
        PPR_TEST_ASSERT(std::abs(tangent.x + 0.7071067811865476) <= 1.0e-9);
        PPR_TEST_ASSERT(std::abs(tangent.y - 0.7071067811865476) <= 1.0e-9);

        // The shipped weights are the separable bilinear-Sobel sequence, pinned at a
        // sub-cell centre: at p = (4.2, 4.7) the same field's gradient is exactly
        // 3.5638181771802, while the cell-centre value is 4.242640687119285. A
        // cell-snapped stencil produces the latter at every offset, so pinning both
        // literals holds the separable fraction weighting in place; the values come
        // from the independent nine-bilinear-sample Sobel of the same field.
        const FlowPoint offset_centre{4.7, 5.2};
        FlowPoint offset_tangent{};
        double offset_gradient = 0.0;
        PPR_TEST_ASSERT(gridParticleSurfaceTangent(body_lower_left, offset_centre, offset_tangent, &offset_gradient));
        PPR_TEST_ASSERT(std::abs(centre_gradient - 4.242640687119285) <= 1.0e-12);
        PPR_TEST_ASSERT(std::abs(offset_gradient - 3.563818177180200) <= 1.0e-12);
        PPR_TEST_ASSERT(offset_gradient < centre_gradient);
        // Not axis-snapped, and it still follows the same surface at the offset.
        PPR_TEST_ASSERT(std::abs(offset_tangent.x + 0.7071067811865476) <= 1.0e-12);
        PPR_TEST_ASSERT(std::abs(offset_tangent.y - 0.7071067811865476) <= 1.0e-12);
        PPR_TEST_ASSERT(std::abs(offset_tangent.x - tangent.x) <= 1.0e-12);

        // The field is the fill, so fractional levels give the same direction with a
        // proportionally smaller gradient: a 0.75/0.25 diagonal is exactly half the
        // full-fill one, and a one-quantum 0.25 step is a quarter of it. This is the
        // reason the fill replaced the mask: one Q16 quantum is 16384 of the word's
        // 65536 units, a quarter of full scale (0.25 cell-volume), not a whole bit.
        const auto fill_lower_left = [](const int x, const int y) { return x + y <= 9 ? 0.75 : 0.25; };
        const auto quarter_step_left = [](const int x, const int) { return x <= 4 ? 0.25 : 0.0; };
        FlowPoint fill_tangent{};
        double fill_gradient = 0.0;
        PPR_TEST_ASSERT(gridParticleSurfaceTangent(fill_lower_left, centre, fill_tangent, &fill_gradient));
        PPR_TEST_ASSERT(std::abs(fill_gradient - 0.5 * centre_gradient) <= 1.0e-12);
        PPR_TEST_ASSERT(std::abs(fill_tangent.x - tangent.x) <= 1.0e-12);
        PPR_TEST_ASSERT(std::abs(fill_tangent.y - tangent.y) <= 1.0e-12);
        PPR_TEST_ASSERT(std::abs(gridParticleLength(fill_tangent) - 1.0) <= 1.0e-12);

        FlowPoint quantum_tangent{};
        double quantum_gradient = 0.0;
        PPR_TEST_ASSERT(gridParticleSurfaceTangent(quarter_step_left, centre, quantum_tangent, &quantum_gradient));
        // A vertical step scales linearly with its amplitude, so the quarter step is
        // a quarter of the full-fill vertical gradient -- not of the diagonal one,
        // whose stencil differs.
        PPR_TEST_ASSERT(std::abs(quantum_gradient - 0.25 * left_gradient) <= 1.0e-12);
        PPR_TEST_ASSERT(std::abs(quantum_tangent.y - 1.0) <= 1.0e-12);

        // A 0.25 -> 1.0 fill ramp across three cells gives one constant, unit-length
        // tangent along the whole ramp, which is the probe the plan asks for. The
        // sample stays inside the linear stretch of the ramp so the 4x4 window never
        // touches its ends; because the field is linear in x the Sobel gradient is
        // exactly 2.0 there whatever the sub-cell offset.
        const auto fill_ramp = [](const int x, const int) { return 0.25 * double(x - 2); };
        FlowPoint ramp_reference{};
        PPR_TEST_ASSERT(gridParticleSurfaceTangent(fill_ramp, FlowPoint{3.5, 5.3}, ramp_reference));
        for (u32 index = 0u; index <= 40u; ++index) {
            const FlowPoint sample{3.5 + 0.07 * double(index), 4.2 + 0.06 * double(index % 7u)};
            FlowPoint ramp_tangent{};
            double ramp_gradient = 0.0;
            PPR_TEST_ASSERT(gridParticleSurfaceTangent(fill_ramp, sample, ramp_tangent, &ramp_gradient));

            PPR_TEST_ASSERT(std::abs(ramp_gradient - 2.0) <= 1.0e-12);
            PPR_TEST_ASSERT(std::abs(ramp_tangent.x - ramp_reference.x) <= 1.0e-12);
            PPR_TEST_ASSERT(std::abs(ramp_tangent.y - ramp_reference.y) <= 1.0e-12);
            PPR_TEST_ASSERT(std::abs(gridParticleLength(ramp_tangent) - 1.0) <= 1.0e-12);
        }
        PPR_TEST_ASSERT(std::abs(ramp_reference.x) <= 1.0e-12 and std::abs(ramp_reference.y + 1.0) <= 1.0e-12);

        // Field-update bound: moving one native cell's fill changes the tangent, and
        // this is the honest, measured size of that step. The probe raises one cell
        // of the ramp by one particle volume (0.25) - the fill quantum - over a 0.02
        // cell grid; the worst sample is the cell centre (4.5, 4.5) at 0.24498 rad
        // (14.0 degrees). The binary mask's single-bit flip on the same probe is
        // 0.76617 rad, 3.1x larger, which is why the fill field replaces it.
        // What the eye sees is the contour step, not the rotation: dk =
        // k0*b*|sin(dtheta)|, i.e. 0.08083 here against 0.23107 for the mask at
        // the maximum bias. The energy gate only shrinks these: the ramp has
        // |grad| = 2 (strength 0.5) while the full-fill mask step has strength 1,
        // so these remain conservative upper bounds and the comparison holds.
        const auto ramp_flipped = [](const int x, const int y) {
            return x == 4 and y == 5 ? 0.75 : 0.25 * double(x - 2);
        };
        const auto mask_step = [](const int x, const int) { return x >= 4 ? 1.0 : 0.0; };
        const auto mask_flipped = [](const int x, const int y) {
            return x == 4 and y == 5 ? 0.0 : (x >= 4 ? 1.0 : 0.0);
        };
        const auto worst_rotation = [](const auto &reference, const auto &perturbed) {
            double worst = 0.0;
            for (u32 row = 0u; row <= 300u; ++row) {
                for (u32 column = 0u; column <= 300u; ++column) {
                    const FlowPoint sample{2.0 + 0.02 * double(column), 2.0 + 0.02 * double(row)};
                    FlowPoint before{};
                    FlowPoint after{};
                    if (not gridParticleSurfaceTangent(reference, sample, before) or
                        not gridParticleSurfaceTangent(perturbed, sample, after))
                        continue;

                    worst = (std::max) (worst,
                        gridParticleAngleDifference(gridParticleFlowAngle(after), gridParticleFlowAngle(before)));
                }
            }
            return worst;
        };
        const double fill_rotation = worst_rotation(fill_ramp, ramp_flipped);
        const double mask_rotation = worst_rotation(mask_step, mask_flipped);
        const double contour_bias_product = double(kGridParticleTangentContourScale) *
                                            double(kGridParticleTangentContourBias);
        const double fill_contour_step = contour_bias_product * std::abs(std::sin(fill_rotation));
        const double mask_contour_step = contour_bias_product * std::abs(std::sin(mask_rotation));
        PPR_TEST_ASSERT(std::abs(fill_rotation - 0.24498) <= 1.0e-4);
        PPR_TEST_ASSERT(std::abs(mask_rotation - 0.76617) <= 1.0e-4);
        PPR_TEST_ASSERT(fill_contour_step <= 0.081);
        PPR_TEST_ASSERT(mask_contour_step >= 2.5 * fill_contour_step);
        // The tangent stays unit length under the update, so the bounded step is a
        // rotation of the same silhouette rather than a change of its shape family.
        FlowPoint after_flip{};
        PPR_TEST_ASSERT(gridParticleSurfaceTangent(ramp_flipped, FlowPoint{4.5, 4.5}, after_flip));
        PPR_TEST_ASSERT(std::abs(gridParticleLength(after_flip) - 1.0) <= 1.0e-12);

        // The painted silhouette follows that tangent: at the same radius the
        // alpha is larger along the tangent than across it, and rotating the
        // surface rotates the long axis.
        const FlowPoint left_tangent{0.0, 1.0};
        const FlowPoint below_tangent{-1.0, 0.0};
        const double radius = 0.9 * support;
        const FlowPoint along{0.0, radius};
        const FlowPoint across{radius, 0.0};
        PPR_TEST_ASSERT(
            gridParticleShadedFalloff(along, left_tangent, true) >
            gridParticleShadedFalloff(across, left_tangent, true));
        PPR_TEST_ASSERT(
            gridParticleShadedFalloff(across, below_tangent, true) >
            gridParticleShadedFalloff(along, below_tangent, true));

        // The contour function is total on its own domain: the exact centre (where
        // |local| == 0) is the plateau and the support is the endpoint, so a future
        // reuse cannot evaluate the warp outside the band. The support is closed by
        // the join-closure hair because |local| is a rounded length.
        PPR_TEST_ASSERT(kGridParticleTangentJoinClosure > 0.0f and kGridParticleTangentJoinClosure < 1.0e-3f);
        PPR_TEST_ASSERT(gridParticleTangentFalloff(FlowPoint{0.0, 0.0}, left_tangent) == 1.0);
        PPR_TEST_ASSERT(gridParticleTangentFalloff(FlowPoint{support, 0.0}, left_tangent) == 0.0);
        PPR_TEST_ASSERT(gridParticleTangentFalloff(FlowPoint{1.5 * support, 0.0}, left_tangent) == 0.0);

        // Flat fields have no surface: the dense interior (at any fill level), the
        // empty exterior and an isolated particle keep today's falloff byte for
        // byte, and the fragment's zero-tangent path is a no-op.
        const auto flat_empty = [](const int, const int) { return 0.0; };
        const auto flat_full = [](const int, const int) { return 1.0; };
        const auto flat_quarter = [](const int, const int) { return 0.25; };
        const auto isolated = [](const int x, const int y) { return x == 4 and y == 5 ? 1.0 : 0.0; };
        const auto expect_no_tangent = [&](const auto &field) {
            FlowPoint none{};
            PPR_TEST_ASSERT(not gridParticleSurfaceTangent(field, centre, none));
            PPR_TEST_ASSERT(none.x == 0.0 and none.y == 0.0);

            constexpr std::array<double, 6u> radii{0.3, 0.5, 0.6666667, 0.8, 0.9, 1.0};
            for (const double rho: radii) {
                const FlowPoint local{rho * support, 0.0};
                const u32 resting = std::bit_cast<u32>(gridParticleFalloff(static_cast<float>(gridParticleRho(local))));

                PPR_TEST_ASSERT(std::bit_cast<u32>(gridParticleShadedFalloff(local, none, false)) == resting);
            }
        };
        expect_no_tangent(flat_empty);
        expect_no_tangent(flat_full);
        expect_no_tangent(flat_quarter);
        expect_no_tangent(isolated);

        // The no-op assertions above cannot pass vacuously: a real tangent does
        // change the same sample.
        const FlowPoint sample{0.0, 1.5};
        const u32 resting_sample = std::bit_cast<u32>(gridParticleFalloff(static_cast<float>(gridParticleRho(sample))));
        PPR_TEST_ASSERT(std::bit_cast<u32>(gridParticleShadedFalloff(sample, left_tangent, true)) != resting_sample);

        // Deterministic: the same field and centre produce the same tangent bit
        // for bit. The stencil weights are fixed by the centre, so the sums have a
        // fixed evaluation order, and at a cell centre they reduce to the exact
        // +/-1/+/-2 integer taps over the 0/1 field.
        FlowPoint first_tangent{};
        FlowPoint second_tangent{};
        PPR_TEST_ASSERT(gridParticleSurfaceTangent(body_lower_left, centre, first_tangent));
        PPR_TEST_ASSERT(gridParticleSurfaceTangent(body_lower_left, centre, second_tangent));
        PPR_TEST_ASSERT(
            std::bit_cast<u64>(first_tangent.x) == std::bit_cast<u64>(second_tangent.x) and
            std::bit_cast<u64>(first_tangent.y) == std::bit_cast<u64>(second_tangent.y));

        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL particle_tangent_field left=({:.4f},{:.4f}) diagonal=({:.4f},{:.4f}) "
                "offset=({:.4f},{:.4f}) centre_gradient={:.4f} offset_gradient={:.4f} fill_gradient={:.4f} "
                "quantum_gradient={:.4f} ramp=({:.4f},{:.4f}) fill_rotation={:.5f} mask_rotation={:.5f} "
                "fill_contour_step={:.5f} mask_contour_step={:.5f}",
                left_tangent.x, left_tangent.y, first_tangent.x, first_tangent.y, offset_tangent.x, offset_tangent.y,
                centre_gradient, offset_gradient, fill_gradient, quantum_gradient, ramp_reference.x, ramp_reference.y,
                fill_rotation, mask_rotation, fill_contour_step, mask_contour_step));
    };

    PPR_UNIT_TEST (particle_tangent_energy_gate_fades_weak_fields) {
        // P2 stability, stage 1: the contour bias is scaled by an energy gate on
        // the unnormalised fill-gradient magnitude, so a genuine surface keeps the
        // full b while the ambiguous fields that flip render round. The two edges
        // are proposed calibration values, not measured thresholds.
        PPR_TEST_ASSERT(kGridParticleTangentConfidenceLow == 1.0f);
        PPR_TEST_ASSERT(kGridParticleTangentConfidenceHigh == 3.0f);
        PPR_TEST_ASSERT(kGridParticleTangentConfidenceLow < kGridParticleTangentConfidenceHigh);

        // The gate itself: 0 at the quarter-fill step (|grad| = 1) and below, half
        // at |grad| = 2, and 1 from a full-fill step (|grad| = 4) upward.
        PPR_TEST_ASSERT(gridParticleTangentStrength(0.0) == 0.0);
        PPR_TEST_ASSERT(gridParticleTangentStrength(0.25) == 0.0);
        PPR_TEST_ASSERT(gridParticleTangentStrength(1.0) == 0.0);
        PPR_TEST_ASSERT(std::abs(gridParticleTangentStrength(2.0) - 0.5) <= 1.0e-12);
        PPR_TEST_ASSERT(gridParticleTangentStrength(3.0) == 1.0);
        PPR_TEST_ASSERT(gridParticleTangentStrength(4.0) == 1.0);

        const FlowPoint centre{4.5, 5.5};
        const auto body_left = [](const int x, const int) { return x <= 4 ? 1.0 : 0.0; };
        const auto quarter_step_left = [](const int x, const int) { return x <= 4 ? 0.25 : 0.0; };
        const auto fill_lower_left = [](const int x, const int y) { return x + y <= 9 ? 0.75 : 0.25; };
        const auto fill_ramp = [](const int x, const int) { return 0.25 * double(x - 2); };
        const auto shallow_ramp = [](const int x, const int) { return (1.0 / 32.0) * double(x - 2); };

        FlowPoint tangent{};
        double gradient = 0.0;
        double strength = 0.0;

        // A full-fill step is a real surface: |grad| = 4, full strength and the
        // existing unit tangent.
        PPR_TEST_ASSERT(gridParticleSurfaceTangent(body_left, centre, tangent, &gradient, &strength));
        PPR_TEST_ASSERT(std::abs(gradient - 4.0) <= 1.0e-12);
        PPR_TEST_ASSERT(strength == 1.0);

        // A quarter-fill step has |grad| = 1, exactly the low edge: the direction is
        // still measurable, but the bias is zero, so the contour stays the resting
        // k0 instead of retargeting an untrusted axis.
        PPR_TEST_ASSERT(gridParticleSurfaceTangent(quarter_step_left, centre, tangent, &gradient, &strength));
        PPR_TEST_ASSERT(std::abs(gradient - 1.0) <= 1.0e-12);
        PPR_TEST_ASSERT(strength == 0.0);

        // A shallow 1/32 ramp is |grad| = 0.25: gated off as well.
        PPR_TEST_ASSERT(gridParticleSurfaceTangent(shallow_ramp, FlowPoint{3.5, 5.3}, tangent, &gradient, &strength));
        PPR_TEST_ASSERT(std::abs(gradient - 0.25) <= 1.0e-12);
        PPR_TEST_ASSERT(strength == 0.0);

        // The partial cases sit on the ramp: the 0.75/0.25 diagonal is 2.12132 and
        // the 0.25-per-cell ramp is 2.0, giving strengths 0.590544 and 0.5.
        PPR_TEST_ASSERT(gridParticleSurfaceTangent(fill_lower_left, centre, tangent, &gradient, &strength));
        PPR_TEST_ASSERT(std::abs(gradient - 4.242640687119285 / 2.0) <= 1.0e-12);
        PPR_TEST_ASSERT(std::abs(strength - 0.5905438404862061) <= 1.0e-12);
        PPR_TEST_ASSERT(gridParticleSurfaceTangent(fill_ramp, FlowPoint{3.5, 5.3}, tangent, &gradient, &strength));
        PPR_TEST_ASSERT(std::abs(gradient - 2.0) <= 1.0e-12);
        PPR_TEST_ASSERT(strength == 0.5);

        // b_eff = b * strength never exceeds b, and at strength 0 the contour is
        // exactly k0 for every direction: the cue becomes the resting circle.
        const float k0 = kGridParticleTangentContourScale;
        for (const float cosine_squared: {0.0f, 0.25f, 0.5f, 1.0f}) {
            PPR_TEST_ASSERT(gridParticleTangentContour(cosine_squared, 0.0f) == k0);
        }
        PPR_TEST_ASSERT(gridParticleTangentContour(1.0f, 1.0f) == gridParticleTangentContourUnclamped(1.0f));
        const float half_bias = gridParticleTangentContour(1.0f, 0.5f);
        PPR_TEST_ASSERT(half_bias > k0 and half_bias < gridParticleTangentContour(1.0f));

        // The painted band at strength 0 is the resting radial falloff (within the
        // binary32 k0 quantisation), so an ambiguous field does not open a new
        // shape; it only loses the angular retarget.
        const FlowPoint outer{0.0, 0.9 * double(kGridParticleSupportScale)};
        PPR_TEST_ASSERT(
            std::abs(gridParticleShadedFalloff(outer, FlowPoint{1.0, 0.0}, true, 0.0f) - gridParticleFalloff(0.9f)) <=
            1.0e-6f);

        // Deterministic: the gate is a pure function of the same field.
        double first_strength = 0.0;
        double second_strength = 0.0;
        FlowPoint first_tangent{};
        FlowPoint second_tangent{};
        PPR_TEST_ASSERT(gridParticleSurfaceTangent(body_left, centre, first_tangent, nullptr, &first_strength));
        PPR_TEST_ASSERT(gridParticleSurfaceTangent(body_left, centre, second_tangent, nullptr, &second_strength));
        PPR_TEST_ASSERT(std::bit_cast<u64>(first_strength) == std::bit_cast<u64>(second_strength));
        PPR_TEST_ASSERT(std::bit_cast<u64>(first_tangent.x) == std::bit_cast<u64>(second_tangent.x));

        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL particle_tangent_gate low={:.3f} high={:.3f} full={:.4f} "
                "quarter={:.4f} diagonal={:.6f} ramp={:.4f}",
                kGridParticleTangentConfidenceLow, kGridParticleTangentConfidenceHigh, gridParticleTangentStrength(4.0),
                gridParticleTangentStrength(1.0), gridParticleTangentStrength(4.242640687119285 / 2.0),
                gridParticleTangentStrength(2.0)));
    };

    PPR_UNIT_TEST (particle_tangent_gate_sequence_reduces_weak_flips_only) {
        // Mechanism evidence, not scene statistics: two scripted synthetic
        // sequences at one fixed sample point, over consecutive "publications".
        // The weak sequence models the owner-reported failure (the only gradient
        // is a particle-sized quantum whose axis flips between x and y); the strong
        // sequence models a real surface whose axis still flips between two
        // coherent publications. Displacement uses the maximum over sample
        // directions of the half-coverage contour move,
        //   dk = k0 * (b_i + b_j)/2 * max_theta |cos^2(theta-axis_j) -
        //                                            cos^2(theta-axis_i)|,
        // where the maximum is |sin(axis angle)| for two unoriented axes (1 for
        // the 90-degree flips here). It does not establish that the owner's scene
        // is weak-gradient dominated, and it does not bound temporal acceptance.
        constexpr double k0 = double(kGridParticleTangentContourScale);
        const double unconditional_bias = double(kGridParticleTangentContourBias);
        constexpr double r_cell = 0.28209479177387814; // sqrt(0.25/pi) for volume 0.25
        const double r_px_span64 = double(kGridParticleSupportScale) * r_cell * (720.0 / 64.0);
        PPR_TEST_ASSERT(std::abs(r_px_span64 - 6.347) <= 1.0e-3);

        struct Step {
            double m_gradient{};
            double m_strength{};
            float m_bias{};
            FlowPoint m_tangent{};
        };

        const FlowPoint centre{4.5, 5.5};
        const auto weak_x = [](const int x, const int y) {
            double value = 0.25;
            if (x == 5 and y == 5)
                value = 0.5;
            else if (x == 3 and y == 5)
                value = 0.0;
            return value;
        };
        const auto weak_y = [](const int x, const int y) {
            double value = 0.25;
            if (x == 4 and y == 6)
                value = 0.5;
            else if (x == 4 and y == 4)
                value = 0.0;
            return value;
        };
        const auto body_left = [](const int x, const int) { return x <= 4 ? 1.0 : 0.0; };
        const auto body_below = [](const int, const int y) { return y <= 5 ? 1.0 : 0.0; };

        const auto sample = [&](const auto &field) {
            Step step{};
            PPR_TEST_ASSERT(
                gridParticleSurfaceTangent(field, centre, step.m_tangent, &step.m_gradient, &step.m_strength));
            step.m_bias = kGridParticleTangentContourBias * static_cast<float>(step.m_strength);
            return step;
        };
        const auto axis_sine = [](const Step &left, const Step &right) {
            const double cross = left.m_tangent.x * right.m_tangent.y - left.m_tangent.y * right.m_tangent.x;
            return (std::min) (std::abs(cross), 1.0);
        };
        const auto displacement = [&](const Step &left, const Step &right, const bool gated) {
            const double left_bias = gated ? double(left.m_bias) : unconditional_bias;
            const double right_bias = gated ? double(right.m_bias) : unconditional_bias;
            return k0 * 0.5 * (left_bias + right_bias) * axis_sine(left, right);
        };
        const auto worst_step = [&](const auto &steps, const bool gated) {
            double worst = 0.0;
            for (u32 index = 1u; index < steps.size(); ++index)
                worst = (std::max) (worst, displacement(steps[index - 1u], steps[index], gated));
            return worst;
        };

        // Weak-field failure mode: eight publications alternating an x pair and a
        // y pair of +/-0.25 cells (one fill quantum) around a 0.25 baseline. The
        // raw gradient is exactly 1.0 in every step, so the gate's strength is 0
        // and b_eff is 0: the resting round contour in every publication.
        std::array<Step, 8u> weak{};
        for (u32 index = 0u; index < weak.size(); ++index) {
            weak[index] = index % 2u == 0u ? sample(weak_x) : sample(weak_y);
            PPR_TEST_ASSERT(std::abs(weak[index].m_gradient - 1.0) <= 1.0e-12);
            PPR_TEST_ASSERT(weak[index].m_strength <= 0.1);
            PPR_TEST_ASSERT(weak[index].m_bias == 0.0f);
        }

        // Strong-field documented residual: six publications of full-fill coherent
        // half-planes, alternating the surface axis by 90 degrees (this is also a
        // three-step coherent sequence). Every step has |grad| = 4, strength 1 and
        // b_eff bit-identical to the constant bias, so the gate cannot change a
        // real surface.
        std::array<Step, 6u> strong{};
        for (u32 index = 0u; index < strong.size(); ++index) {
            strong[index] = index % 2u == 0u ? sample(body_left) : sample(body_below);
            PPR_TEST_ASSERT(std::abs(strong[index].m_gradient - 4.0) <= 1.0e-12);
            PPR_TEST_ASSERT(strong[index].m_strength == 1.0);
            PPR_TEST_ASSERT(
                std::bit_cast<u32>(strong[index].m_bias) == std::bit_cast<u32>(kGridParticleTangentContourBias));
        }

        const double weak_before = worst_step(weak, false);
        const double weak_after = worst_step(weak, true);
        const double strong_before = worst_step(strong, false);
        const double strong_after = worst_step(strong, true);
        const double unconditional = k0 * unconditional_bias;

        // `k0 * b` is the unclamped proxy for a 90-degree axis flip; the shipped
        // contour is clamped at the minor endpoint, so the movement the eye sees is
        // the span of the clamped contour, which is slightly smaller and is what the
        // proxy bounds.
        const float clamped_flip = gridParticleTangentContour(1.0f, 1.0f) - gridParticleTangentContour(0.0f, 1.0f);
        PPR_TEST_ASSERT(std::abs(double(clamped_flip) - 0.33319163) <= 1.0e-7);
        PPR_TEST_ASSERT(double(clamped_flip) <= unconditional);

        // Before the gate any 90-degree flip moved the contour by k0*b. The gate
        // reduces the weak sequence to zero (strength 0 throughout), and leaves
        // the strong sequence at exactly the old worst case: only cell history can
        // bound the latter, so this test does not claim closure for it.
        PPR_TEST_ASSERT(std::abs(weak_before - unconditional) <= 1.0e-9);
        PPR_TEST_ASSERT(std::abs(strong_before - unconditional) <= 1.0e-9);
        PPR_TEST_ASSERT(weak_after <= 0.0034);
        PPR_TEST_ASSERT(std::abs(strong_after - unconditional) <= 1.0e-9);

        for (u32 index = 0u; index < weak.size(); ++index) {
            const double before = index == 0u ? 0.0 : displacement(weak[index - 1u], weak[index], false);
            const double after = index == 0u ? 0.0 : displacement(weak[index - 1u], weak[index], true);
            _.log(
                std::format(
                    "WORLD_LIGHTING_MATERIAL particle_tangent_sequence weak step={} G={:.6f} strength={:.6f} "
                    "b_eff={:.6f} axis=({:.4f},{:.4f}) axis_turn={:.6f} before_R={:.6f} after_R={:.6f}",
                    index, weak[index].m_gradient, weak[index].m_strength, double(weak[index].m_bias),
                    weak[index].m_tangent.x, weak[index].m_tangent.y,
                    std::asin(axis_sine(index == 0u ? weak[index] : weak[index - 1u], weak[index])), before, after));
        }
        for (u32 index = 0u; index < strong.size(); ++index) {
            const double before = index == 0u ? 0.0 : displacement(strong[index - 1u], strong[index], false);
            const double after = index == 0u ? 0.0 : displacement(strong[index - 1u], strong[index], true);
            _.log(
                std::format(
                    "WORLD_LIGHTING_MATERIAL particle_tangent_sequence strong step={} G={:.6f} strength={:.6f} "
                    "b_eff={:.6f} axis=({:.4f},{:.4f}) axis_turn={:.6f} before_R={:.6f} after_R={:.6f}",
                    index, strong[index].m_gradient, strong[index].m_strength, double(strong[index].m_bias),
                    strong[index].m_tangent.x, strong[index].m_tangent.y,
                    std::asin(axis_sine(index == 0u ? strong[index] : strong[index - 1u], strong[index])), before,
                    after));
        }

        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL particle_tangent_sequence summary R_px_span64={:.4f} "
                "weak_before_R={:.6f} weak_after_R={:.6f} weak_before_px={:.4f} weak_after_px={:.4f} "
                "strong_before_R={:.6f} strong_after_R={:.6f} strong_before_px={:.4f} strong_after_px={:.4f}",
                r_px_span64, weak_before, weak_after, weak_before * r_px_span64, weak_after * r_px_span64,
                strong_before, strong_after, strong_before * r_px_span64, strong_after * r_px_span64));
    };

    PPR_UNIT_TEST (particle_plateau_coverage_is_conditional) {
        // Reconciles the plan's "opaque in the plateau" claim with the final
        // composite: the radial falloff is exactly 1 for rho <= 2/3, but final solid
        // coverage also multiplies the solid SDF rim and (1 - terrain coverage). The
        // claim is therefore conditional, not universal. No behaviour changes here:
        // the coarse-zoom case is the intended analytic rim, not a defect.
        const float support = kGridParticleSupportScale;
        constexpr float r_cell = 0.28209479177387814f;
        const FlowPoint interior{double(0.2f * support), 0.0};
        const FlowPoint plateau_edge{double((2.0f / 3.0f) * support), 0.0};

        // The radial falloff itself is still exactly 1 at both plateau samples; any
        // coverage below 1 is the rim/terrain multiplier, not a moved plateau.
        PPR_TEST_ASSERT(gridParticleFalloff(0.2f) == 1.0f);
        PPR_TEST_ASSERT(gridParticleFalloff(2.0f / 3.0f) == 1.0f);

        // Conditional claim: at a rim half-width <= 0.15 r (rim width 0.30 r, the
        // fine span64 regime) and zero terrain coverage, final solid coverage is
        // exactly 1 across the radial plateau, including its flat edge.
        constexpr float fine_rim = 0.30f;
        PPR_TEST_ASSERT(0.5f * fine_rim <= 0.15f);
        for (const FlowPoint local: {interior, plateau_edge}) {
            PPR_TEST_ASSERT(gridParticleShapeCoverage(local, fine_rim, false) == 1.0f);
            PPR_TEST_ASSERT(gridParticleEdgeCoverage(local, FlowPoint{}, false, false, fine_rim, 0.0f) == 1.0f);
        }

        // The shipped span64 rim (0.3151 r, half-width 0.1576 r) still saturates the
        // same two samples, so the fine claim is not a special-case constant.
        const float span64_rim = 64.0f / 720.0f / r_cell;
        PPR_TEST_ASSERT(std::abs(span64_rim - 0.3151f) <= 1.0e-4f);
        for (const FlowPoint local: {interior, plateau_edge}) {
            PPR_TEST_ASSERT(gridParticleEdgeCoverage(local, FlowPoint{}, false, false, span64_rim, 0.0f) == 1.0f);
        }

        // Coarse zoom (span220, rim 1.0832 r) is the documented exception: the rim
        // reaches into the plateau. The flat plateau edge is 0.1927 r inside the SDF
        // and is only 0.6779 covered; rho = 0.5 is 0.9856; the deep interior still
        // saturates. Asserted rather than pretending the unconditional claim holds.
        const float span220_rim = 220.0f / 720.0f / r_cell;
        PPR_TEST_ASSERT(std::abs(span220_rim - 1.0832f) <= 1.0e-4f);
        PPR_TEST_ASSERT(
            std::abs(gridParticleEdgeCoverage(plateau_edge, FlowPoint{}, false, false, span220_rim, 0.0f) - 0.67789f) <=
            1.0e-4f);
        PPR_TEST_ASSERT(
            std::abs(
                gridParticleEdgeCoverage(
                    FlowPoint{double(0.5f * support), 0.0}, FlowPoint{}, false, false, span220_rim, 0.0f) -
                0.98563f) <= 1.0e-4f);
        PPR_TEST_ASSERT(gridParticleEdgeCoverage(interior, FlowPoint{}, false, false, span220_rim, 0.0f) == 1.0f);

        // The terrain complement is the second multiplier: even deep in the radial
        // plateau, partial terrain coverage makes final coverage partial by exactly
        // (1 - terrain coverage).
        PPR_TEST_ASSERT(gridParticleEdgeCoverage(interior, FlowPoint{}, false, false, fine_rim, 0.5f) == 0.5f);

        // The coarse case is the rim and nothing else: the radial falloff remains
        // exactly 1 there.
        PPR_TEST_ASSERT(gridParticleFalloff(2.0f / 3.0f) == 1.0f);
        PPR_TEST_ASSERT(gridParticleShapeCoverage(plateau_edge, span220_rim, false) < 1.0f);

        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL particle_plateau fine_rim={:.4f} span64={:.4f} span220={:.4f} "
                "edge_span220={:.5f} mid_span220={:.5f}",
                fine_rim, span64_rim, span220_rim,
                gridParticleEdgeCoverage(plateau_edge, FlowPoint{}, false, false, span220_rim, 0.0f),
                gridParticleEdgeCoverage(
                    FlowPoint{double(0.5f * support), 0.0}, FlowPoint{}, false, false, span220_rim, 0.0f)));
    };

    PPR_UNIT_TEST (particle_solid_rim_is_analytic_coverage) {
        // The solid's rounded-square SDF is a coverage term now, not a binary
        // reject: flats and corners cross the boundary through the same one-pixel
        // ramp, while a liquid keeps the falloff disk unchanged. The rim width
        // below is one pixel of the 720-px review raster at span64
        // (64/720 cell = 0.3151 r). The shader derives that width from
        // ddx/ddy(local) and never hardcodes it; at span220 one pixel is 1.0832 r.
        constexpr float r_cell = 0.28209479177387814f;
        const float rim_width = 64.0f / 720.0f / r_cell;
        const float support = kGridParticleSupportScale;
        const float half_extent = kGridParticleSolidHalfExtentScale;
        const double diagonal = 1.0 / std::sqrt(2.0);

        // The flat boundary is the SDF zero at |x| = a, i.e. rho = a / 2 = 0.763.
        // The old hard cut dropped the resting falloff 0.7977 there in one step.
        const FlowPoint flat_boundary{double(half_extent), 0.0};
        const float flat_rho = half_extent / support;
        const float resting_falloff = gridParticleFalloff(flat_rho);
        PPR_TEST_ASSERT(std::abs(gridParticleSolidDistance(flat_boundary)) <= 1.0e-6f);
        PPR_TEST_ASSERT(std::abs(flat_rho - 0.763f) <= 1.0e-4f);
        PPR_TEST_ASSERT(std::abs(resting_falloff - 0.7977f) <= 1.0e-4f);

        // On the boundary the ramp is exactly half: 0.5 * 0.7977, where the old
        // rule produced the full 0.7977 -> 0 step.
        const float boundary_shape = gridParticleShapeCoverage(flat_boundary, rim_width, false);
        const float boundary_coverage = gridParticleEdgeCoverage(
            flat_boundary, FlowPoint{}, false, false, rim_width, 0.0f);
        PPR_TEST_ASSERT(boundary_shape == 0.5f);
        PPR_TEST_ASSERT(std::abs(boundary_coverage - 0.5f * resting_falloff) <= 1.0e-5f);
        PPR_TEST_ASSERT(std::abs(boundary_coverage - 0.39884f) <= 1.0e-4f);

        // One pixel across the flat: still full on the inner side, half on the
        // boundary, and below the depth-write floor on the outer side. The step
        // is distributed over the ramp instead of quantised to MSAA levels.
        const auto flat_at = [&](const float offset_px) {
            return FlowPoint{double(half_extent) + double(offset_px) * double(rim_width), 0.0};
        };
        const float inner_shape = gridParticleShapeCoverage(flat_at(-0.5f), rim_width, false);
        const float inner_coverage = gridParticleEdgeCoverage(
            flat_at(-0.5f), FlowPoint{}, false, false, rim_width, 0.0f);
        const float outer_shape = gridParticleShapeCoverage(flat_at(0.5f), rim_width, false);
        const float outer_coverage = gridParticleEdgeCoverage(
            flat_at(0.5f), FlowPoint{}, false, false, rim_width, 0.0f);
        PPR_TEST_ASSERT(inner_shape >= 0.9999f);
        PPR_TEST_ASSERT(inner_coverage >= 0.99f);
        PPR_TEST_ASSERT(outer_shape <= 1.0e-6f);
        PPR_TEST_ASSERT(outer_coverage <= kGridParticleCoverageFloor);
        PPR_TEST_ASSERT(inner_coverage > boundary_coverage and boundary_coverage > outer_coverage);

        // The whole ramp is monotone across that pixel and reaches exactly 0 one
        // footprint outside the boundary, so no re-brightening survives the cut.
        float previous_flat = gridParticleEdgeCoverage(flat_at(-1.0f), FlowPoint{}, false, false, rim_width, 0.0f);
        for (u32 step = 1u; step <= 8u; ++step) {
            const float offset_px = -1.0f + 2.0f * static_cast<float>(step) / 8.0f;
            const float coverage = gridParticleEdgeCoverage(
                flat_at(offset_px), FlowPoint{}, false, false, rim_width, 0.0f);
            PPR_TEST_ASSERT(coverage <= previous_flat);
            previous_flat = coverage;
        }
        PPR_TEST_ASSERT(previous_flat == 0.0f);

        // Deep inside the solid (rho = 0.2, on an axis and in the corner region)
        // the ramp saturates to exactly 1.0 in float32, so the composite is
        // bit-identical to the pre-change resting falloff.
        const FlowPoint plateau_axis{double(0.2f * support), 0.0};
        const FlowPoint plateau_corner{double(0.2f * support) * diagonal, double(0.2f * support) * diagonal};
        for (const FlowPoint plateau: {plateau_axis, plateau_corner}) {
            PPR_TEST_ASSERT(gridParticleShapeCoverage(plateau, rim_width, false) == 1.0f);
            const float expected = gridParticleShadedFalloff(plateau, FlowPoint{}, false);
            const float coverage = gridParticleEdgeCoverage(plateau, FlowPoint{}, false, false, rim_width, 0.0f);
            PPR_TEST_ASSERT(std::bit_cast<u32>(coverage) == std::bit_cast<u32>(expected));
        }
        PPR_TEST_ASSERT(gridParticleFalloff(0.2f) == 1.0f);

        // The identity holds wherever the ramp saturates, not only on the
        // plateau: at rho = 0.9 on the corner ray the falloff is already below 1
        // but the shape term is still exactly 1.
        const FlowPoint interior{double(0.9f * support) * diagonal, double(0.9f * support) * diagonal};
        const float interior_falloff = gridParticleShadedFalloff(interior, FlowPoint{}, false);
        PPR_TEST_ASSERT(gridParticleShapeCoverage(interior, rim_width, false) == 1.0f);
        PPR_TEST_ASSERT(interior_falloff > 0.0f and interior_falloff < 1.0f);
        PPR_TEST_ASSERT(
            std::bit_cast<u32>(gridParticleEdgeCoverage(interior, FlowPoint{}, false, false, rim_width, 0.0f)) ==
            std::bit_cast<u32>(interior_falloff));

        // At and beyond R the falloff is exactly 0, so the coverage is 0 and the
        // floor is the only cut: the support did not move.
        for (const float rho: {1.0f, 1.05f}) {
            const FlowPoint local{double(rho * support), 0.0};
            PPR_TEST_ASSERT(gridParticleEdgeCoverage(local, FlowPoint{}, false, false, rim_width, 0.0f) == 0.0f);
        }
        const FlowPoint support_corner{double(1.05f * support) * diagonal, double(1.05f * support) * diagonal};
        PPR_TEST_ASSERT(gridParticleEdgeCoverage(support_corner, FlowPoint{}, false, false, rim_width, 0.0f) == 0.0f);

        // Coverage falls monotonically from the centre out through a flat and
        // through a corner, so no flat or diagonal re-brightens before the support.
        const auto assert_descending = [&](const FlowPoint direction) {
            float previous = gridParticleEdgeCoverage(FlowPoint{}, FlowPoint{}, false, false, rim_width, 0.0f);
            PPR_TEST_ASSERT(previous == 1.0f);
            for (u32 step = 1u; step <= 240u; ++step) {
                const float rho = static_cast<float>(step) / 200.0f;
                const FlowPoint local{
                    direction.x * double(rho) * double(support), direction.y * double(rho) * double(support)};
                const float coverage = gridParticleEdgeCoverage(local, FlowPoint{}, false, false, rim_width, 0.0f);
                PPR_TEST_ASSERT(coverage <= previous);
                previous = coverage;
            }
            PPR_TEST_ASSERT(previous == 0.0f);
        };
        assert_descending(FlowPoint{1.0, 0.0});
        assert_descending(FlowPoint{diagonal, diagonal});

        // A liquid has no SDF term: shape coverage is exactly 1 everywhere, so its
        // composite is the pre-change falloff*(1 - terrain) bit for bit.
        for (const float rho: {0.1f, 0.5f, 0.763f, 0.9f, 1.0f}) {
            const FlowPoint local{double(rho * support), 0.0};
            PPR_TEST_ASSERT(gridParticleShapeCoverage(local, rim_width, true) == 1.0f);
            const float expected = gridParticleShadedFalloff(local, FlowPoint{}, false) * 0.75f;
            PPR_TEST_ASSERT(
                std::bit_cast<u32>(gridParticleEdgeCoverage(local, FlowPoint{}, false, true, rim_width, 0.25f)) ==
                std::bit_cast<u32>(expected));
        }

        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL particle_solid_rim flat_rho={:.4f} resting={:.4f} boundary={:.4f} "
                "rim_span64_r={:.4f} rim_span220_r={:.4f} inner={:.4f} outer={:.4f}",
                flat_rho, resting_falloff, boundary_coverage, rim_width, 220.0f / 720.0f / r_cell, inner_coverage,
                outer_coverage));
    };

    PPR_UNIT_TEST (particle_surface_constants_match_revision_five) {
        // Revision 5 section 10.5 numeric contract against independent literals.
        // The shader twins are grid_particle_density.slang's kParticleSurfaceRadiusScale,
        // kParticleDensitySecondMoment, kParticleDensityAmplitude and threshold, plus
        // the retained stretch/bias caps and coverage floor in grid_particle_native.
        // std::sqrt is not constexpr in this standard library, so the mirror
        // evaluates it at run time; the value is the shipped 0.28209479177387814.
        const double r_reference_cells = std::sqrt(0.25 / kPi);
        constexpr double solid_extent_divisor = 0.25 + 0.75 * 1.4142135623730951;
        constexpr double r_over_r = 2.0 / solid_extent_divisor;
        // R is the old solid's AXIS half-extent, not its circumradius.
        PPR_TEST_ASSERT(std::abs(r_over_r - 1.525948558644369) <= 1.0e-12);
        PPR_TEST_ASSERT(std::abs(kDensitySurfaceRadiusScale - r_over_r) <= 1.0e-15);
        PPR_TEST_ASSERT(std::abs(kDensitySurfaceRadiusScale * solid_extent_divisor - 2.0) <= 1.0e-14);
        PPR_TEST_ASSERT(std::abs(kDensitySurfaceRadiusScale * r_reference_cells - 0.430462140908433) <= 1.0e-14);
        // The direct solid silhouette keeps its own R = 2 r until the migration,
        // and both still fit the retained 3.0625 r cull.
        PPR_TEST_ASSERT(kDensityDirectSupportScale == 2.0);

        // A_2 = 10/(7 pi), the second moment of the unit-disk profile. The literal is
        // the independent statement; comparing the mirror to its own defining
        // expression would be tautological.
        PPR_TEST_ASSERT(std::abs(kDensitySecondMoment - 0.454728408833987) <= 1.0e-15);
        PPR_TEST_ASSERT(kDensityProfileIntegral == 0.7);
        PPR_TEST_ASSERT(std::abs(kDensityProfileIntegral * kPi * kDensitySecondMoment - 1.0) <= 1.0e-14);
        // V / R^2 = pi / c^2, so the whole amplitude is pi*A_2/c^2 = 10/(7 c^2).
        // The delta is logged before the assertion so a drift reports both sides.
        constexpr double expected_amplitude = 10.0 / (7.0 * r_over_r * r_over_r);
        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL particle_surface_amplitude actual={:.17g} expected={:.17g} delta={:.3e} "
                "ratio={:.15f}",
                kDensityAmplitude, expected_amplitude, std::abs(kDensityAmplitude - expected_amplitude),
                kDensityAmplitude / expected_amplitude));
        PPR_TEST_ASSERT(std::abs(kDensityAmplitude - expected_amplitude) <= 1.0e-15);
        PPR_TEST_ASSERT(std::abs(kDensityAmplitude - 0.613510744960683) <= 1.0e-15);
        PPR_TEST_ASSERT(std::abs(kDensityPlateau - 2.0 / 3.0) <= 1.0e-16);
        PPR_TEST_ASSERT(std::abs(kDensityBandFraction - (1.0 - kDensityPlateau)) <= 1.0e-15);

        PPR_TEST_ASSERT(kDensityStretchCap == 1.5);
        PPR_TEST_ASSERT(kDensityBiasCap == 0.3999);
        PPR_TEST_ASSERT(kDensityThreshold == 5.0 / 28.0);
        // The decimal literal is the design's 15-digit transcription of 5/28;
        // compare it at the precision it actually carries.
        PPR_TEST_ASSERT(std::abs(kDensityThreshold - 5.0 / 28.0) <= 1.0e-15);
        PPR_TEST_ASSERT(kDensityEpsilon == 1.0e-6);
        PPR_TEST_ASSERT(kDensitySupportClosure == 1.0e-6);
        PPR_TEST_ASSERT(kDensityCoverageFloor == 1.0 / 255.0);
        PPR_TEST_ASSERT(kDensityQuadratureOffset == 0.25);
        PPR_TEST_ASSERT(kDensityCullRadiusScale == 3.0625);
        PPR_TEST_ASSERT(std::abs(kDensityDepthCeiling - 0.9999999403953552) <= 1.0e-16);

        // support_max = stretch_cap * R = 2.288922837966554 r <= 3.0625 r.
        constexpr double support_over_r = kDensityStretchCap * kDensitySurfaceRadiusScale;
        PPR_TEST_ASSERT(std::abs(support_over_r - 2.288922837966554) <= 1.0e-13);
        PPR_TEST_ASSERT(support_over_r <= kDensityCullRadiusScale);
        PPR_TEST_ASSERT(std::abs(kDensityCullRadiusScale - support_over_r - 0.773577162033446) <= 1.0e-12);
        // The direct solid's 1.5x-stretched silhouette is still the cull driver.
        PPR_TEST_ASSERT(kDensityDirectSupportScale * kDensityStretchCap == 3.0);
        PPR_TEST_ASSERT(kDensityDirectSupportScale * kDensityStretchCap <= kDensityCullRadiusScale);

        // Neutral peak and the drawn isolated contour. theta crosses at the
        // solved root 0.8810686303395725, which the design quotes to 15 digits as
        // 0.881068630339572 (5.6e-16 = 5 ulps below the root, |dw/drho| = 2.53
        // there, so the 15-digit literal moves the profile value by 1.4e-15). The
        // value assertion uses the root and the literal is pinned separately.
        const DensityKernel neutral = densityKernel({1.0, 0.0}, 0.0, 0.0);
        constexpr double crossing_rho = 0.8810686303395725;
        PPR_TEST_ASSERT(std::abs(crossing_rho - 0.881068630339572) <= 1.0e-15);
        PPR_TEST_ASSERT(std::abs(densityWeight({0.0, 0.0}, neutral) - 0.613510744960683) <= 1.0e-15);
        PPR_TEST_ASSERT(std::abs(densityWeight({crossing_rho, 0.0}, neutral) - kDensityThreshold) <= 1.0e-15);
        PPR_TEST_ASSERT(std::abs(densityWeight({5.0 / 6.0, 0.0}, neutral) - 0.306755372480341) <= 1.0e-15);
        PPR_TEST_ASSERT(std::abs(densityWeight({5.0 / 6.0, 0.0}, neutral) - kDensityThreshold) > 0.1);
        const double apparent_radius_cells = crossing_rho * r_over_r * r_reference_cells;
        PPR_TEST_ASSERT(std::abs(apparent_radius_cells - 0.379266688903233) <= 1.0e-14);
        PPR_TEST_ASSERT(std::abs(apparent_radius_cells / r_reference_cells - 1.344465406533439) <= 1.0e-13);
        // A physical-radius contour is not selectable by theta: r/R is inside the
        // rho <= 2/3 plateau.
        PPR_TEST_ASSERT(r_reference_cells / (kDensitySurfaceRadiusScale * r_reference_cells) <= kDensityPlateau);

        // Anisotropy mapping endpoints (unchanged cue mapping).
        const DensityKernel extremes = densityKernel({1.0, 0.0}, 1.0, 1.0);
        PPR_TEST_ASSERT(extremes.m_stretch == 1.5);
        PPR_TEST_ASSERT(std::abs(extremes.m_ratio - (1.0 - 0.5 * 0.3999) / (1.0 + 0.5 * 0.3999)) <= 1.0e-15);
        PPR_TEST_ASSERT(extremes.m_ratio <= 1.0 and extremes.m_stretch >= 1.0);

        // The four fixed quadrature offsets of a density pixel.
        double offset_sum_x = 0.0;
        double offset_sum_y = 0.0;
        for (u32 index = 0u; index < 4u; ++index) {
            const Double2 offset = densityQuadratureOffset(index);
            PPR_TEST_ASSERT(std::abs(offset.x) == kDensityQuadratureOffset);
            PPR_TEST_ASSERT(std::abs(offset.y) == kDensityQuadratureOffset);
            offset_sum_x += offset.x;
            offset_sum_y += offset.y;
        }
        PPR_TEST_ASSERT(offset_sum_x == 0.0 and offset_sum_y == 0.0);
        PPR_TEST_ASSERT(densityQuadratureOffset(0u).x == -0.25 and densityQuadratureOffset(0u).y == -0.25);
        PPR_TEST_ASSERT(densityQuadratureOffset(3u).x == 0.25 and densityQuadratureOffset(3u).y == 0.25);

        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL particle_surface_constants R={:.15f}r R_ref={:.15f} A2={:.15f} "
                "amplitude={:.15f} theta={:.15f} support_max={:.15f}r apparent={:.15f}r/{:.15f}cell",
                kDensitySurfaceRadiusScale, kDensitySurfaceRadiusScale * r_reference_cells, kDensitySecondMoment,
                kDensityAmplitude, kDensityThreshold, support_over_r, apparent_radius_cells / r_reference_cells,
                apparent_radius_cells));
    };

    PPR_UNIT_TEST (particle_surface_isolated_neutral_draws_the_apparent_radius) {
        // Revision 5 keeps theta and raises the neutral peak to 0.613510745 by
        // shrinking R. The isolated half-coverage (C = .5) contour is therefore
        // the solved root rho = 0.8810686303395725 (quoted to 15 digits by the
        // design as 0.881068630339572), not the old 5/6, and the old 5R/6 radius
        // is only the half-PEAK point.
        const DensityKernel neutral = densityKernel({1.0, 0.0}, 0.0, 0.0);
        const double peak = densityWeight({0.0, 0.0}, neutral);
        PPR_TEST_ASSERT(std::abs(peak - 0.613510744960683) <= 1.0e-15);
        PPR_TEST_ASSERT(peak > kDensityThreshold);

        constexpr double crossing_rho = 0.8810686303395725;
        PPR_TEST_ASSERT(std::abs(crossing_rho - 0.881068630339572) <= 1.0e-15);
        const double crossing = densityWeight({crossing_rho, 0.0}, neutral);
        PPR_TEST_ASSERT(std::abs(crossing - kDensityThreshold) <= 1.0e-15);
        PPR_TEST_ASSERT(densityWeight({crossing_rho - 1.0e-6, 0.0}, neutral) > kDensityThreshold);
        PPR_TEST_ASSERT(densityWeight({crossing_rho + 1.0e-6, 0.0}, neutral) < kDensityThreshold);
        PPR_TEST_ASSERT(densityOccupancy(peak) == 1.0);
        // The sample decision is binary and exact on both sides of theta: nothing
        // in between, and the support beyond rho = 1 is exactly zero.
        PPR_TEST_ASSERT(densityOccupancy(densityWeight({0.95, 0.0}, neutral)) == 0.0);
        PPR_TEST_ASSERT(densityWeight({1.0, 0.0}, neutral) == 0.0);

        // Half-peak is not theta: raising theta to that value would move the drawn
        // contour and is explicitly not the selected calibration.
        const double half_peak_rho = 5.0 / 6.0;
        const double half_peak = densityWeight({half_peak_rho, 0.0}, neutral);
        PPR_TEST_ASSERT(std::abs(half_peak - 0.306755372480341) <= 1.0e-15);
        PPR_TEST_ASSERT(std::abs(half_peak - 0.5 * peak) <= 1.0e-15);
        PPR_TEST_ASSERT(half_peak > kDensityThreshold);

        // 34.4465% larger than physical r and 11.8931% smaller than the old solid's
        // 0.430462141-cell axis extent.
        constexpr double r_cells = 0.28209479177387814;
        const double apparent_cells = crossing_rho * kDensitySurfaceRadiusScale * r_cells;
        PPR_TEST_ASSERT(std::abs(apparent_cells / r_cells - 1.344465406533439) <= 1.0e-13);
        PPR_TEST_ASSERT(std::abs(apparent_cells / 0.430462140908433 - 1.0 + 0.118931) <= 1.0e-5);

        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL particle_surface_isolated peak={:.15f} theta={:.15f} rho_star={:.15f} "
                "apparent_cells={:.15f} half_peak={:.15f}",
                peak, kDensityThreshold, crossing_rho, apparent_cells, half_peak));
    };

    PPR_UNIT_TEST (particle_surface_integrated_weight_is_the_reference_area) {
        // Two different claims, asserted with the tools each one needs.
        //
        // 1. The CONTINUOUS plane integral of w is exactly V. This is analytic, not
        //    a quadrature result: integrating s over the unit q-disk gives
        //    2*(2/9 + 23/180) = 0.7 in rho form, and A_2 = 1/(0.7 pi) makes
        //    A_2*0.7 pi = 1, so amplitude*0.7*pi*R^2 = V for R = c r. No tolerance
        //    wider than double round-off is needed.
        // 2. The 512-step midpoint sum in `densityIntegral` is NOT exact for this
        //    kernel: w is C1 but not polynomial in x and y, and the circular
        //    support boundary is only first-order accurate per cell, so the sum
        //    has the second-order error |I_h - V| = C h^2 + O(h^3). The probe
        //    therefore asserts the derived second-order relation on a refined pair
        //    and a measured regression bound, instead of reading a fixed 1e-6 as if
        //    the sum were exact.
        PPR_TEST_ASSERT(std::abs(kDensityProfileIntegral * kPi * kDensitySecondMoment - 1.0) <= 1.0e-15);
        PPR_TEST_ASSERT(std::abs(kDensitySecondMoment - 1.0 / (kDensityProfileIntegral * kPi)) <= 1.0e-15);
        PPR_TEST_ASSERT(std::abs(2.0 * (2.0 / 9.0 + 23.0 / 180.0) - kDensityProfileIntegral) <= 1.0e-15);
        constexpr double volume = 0.25;
        constexpr u32 integral_steps = 512u;
        const double radius = std::sqrt(volume / kPi);
        // V = pi r^2 and R = c r, so the whole amplitude is
        // amplitude = V*A_2/R^2 = pi*A_2/c^2 = 10/(7 c^2).
        PPR_TEST_ASSERT(
            std::abs(
                kDensityAmplitude -
                kPi * kDensitySecondMoment / (kDensitySurfaceRadiusScale * kDensitySurfaceRadiusScale)) <= 1.0e-16);
        PPR_TEST_ASSERT(
            std::abs(kDensityAmplitude - 10.0 / (7.0 * kDensitySurfaceRadiusScale * kDensitySurfaceRadiusScale)) <=
            1.0e-15);
        PPR_TEST_ASSERT(
            std::abs(
                kDensityAmplitude *
                    kDensityProfileIntegral *
                    kPi *
                    kDensitySurfaceRadiusScale *
                    kDensitySurfaceRadiusScale *
                    radius *
                    radius -
                volume) <= 1.0e-15);

        const std::array<std::pair<const char *, DensityKernel>, 3u> kernels{{
            {"neutral", densityKernel({1.0, 0.0}, 0.0, 0.0)},
            {"stretched", densityKernel({1.0, 0.0}, 0.0, 1.0)},
            {"biased", densityKernel({0.6, 0.8}, 1.0, 0.0)},
        }};

        // Logged before the assertions so a drift reports both sides.
        {
            const DensityKernel neutral = densityKernel({1.0, 0.0}, 0.0, 0.0);
            const double actual = densityIntegral(neutral, radius, integral_steps);
            _.log(
                std::format(
                    "WORLD_LIGHTING_MATERIAL particle_surface_integral_actual I512={:.17g} expected={:.17g} "
                    "delta={:.3e}",
                    actual, volume, std::abs(actual - volume)));
        }

        for (const auto &[name, kernel]: kernels) {
            // Midpoint sum at h and h/2. For a second-order method
            // I_h - V = C h^2 + O(h^3), so |I_h/2 - V| is at most about
            // |I_h - I_h/2|/3 for these kernels and these step placements; 0.4 admits
            // the O(h^3) boundary term (measured ratio at most 0.159 here). This is a
            // measured convergence check, not a universal bound for every placement.
            const double coarse = densityIntegral(kernel, radius, integral_steps);
            const double fine = densityIntegral(kernel, radius, 2u * integral_steps);
            const double coarse_error = std::abs(coarse - volume);
            const double fine_error = std::abs(fine - volume);
            PPR_TEST_ASSERT(fine_error <= 0.4 * std::abs(coarse - fine));
            PPR_TEST_ASSERT(fine_error <= coarse_error);
            // Measured 512-step relative error over the three kernels is
            // 3.0e-9 / 5.7e-9 / 7.4e-9, so this is a 13x regression bound.
            PPR_TEST_ASSERT(coarse_error <= 1.0e-7 * volume);

            // The four-point variant the splat pass actually performs has its own
            // measured tolerance: 4.8e-5 / 2.6e-5 / 9.0e-6 relative at
            // support/8 and 1.4e-8 / 2.0e-9 / 1.8e-8 at support/64.
            const double support = kDensitySurfaceRadiusScale * radius;
            const double quad_coarse = densityQuadratureIntegral(kernel, radius, support / 8.0);
            const double quad_fine = densityQuadratureIntegral(kernel, radius, support / 64.0);
            const double quad_coarse_relative = std::abs(quad_coarse - volume) / volume;
            const double quad_fine_relative = std::abs(quad_fine - volume) / volume;
            PPR_TEST_ASSERT(quad_coarse_relative <= 2.0e-4);
            PPR_TEST_ASSERT(quad_fine_relative <= 1.0e-5);
            PPR_TEST_ASSERT(quad_fine_relative < quad_coarse_relative or quad_fine_relative <= 1.0e-9);

            _.log(
                std::format(
                    "WORLD_LIGHTING_MATERIAL particle_surface_integral kernel={} V={:.12f} I512={:.12f} "
                    "rel512={:.3e} I1024={:.12f} rel1024={:.3e} refinement={:.3e} quad_R8_rel={:.3e} "
                    "quad_R64_rel={:.3e}",
                    name, volume, coarse, coarse_error / volume, fine, fine_error / volume, std::abs(coarse - fine),
                    quad_coarse_relative, quad_fine_relative));
        }
    };

    PPR_UNIT_TEST (particle_surface_kernel_is_zero_at_and_beyond_the_support) {
        // The compact support is exactly zero from the support radius outward for
        // every orientation. The surface radius is 1.5259486 r, so the maximum
        // reach is 1.5 * R = 2.2889228 r, inside the retained 3.0625 r cull (which
        // still bounds the direct solid's 3.000 r silhouette).
        constexpr double radius = 1.0;
        const double support = kDensitySurfaceRadiusScale * radius;
        const std::array<DensityKernel, 4u> kernels{{
            densityKernel({1.0, 0.0}, 0.0, 0.0),
            densityKernel({1.0, 0.0}, 0.0, 1.0),
            densityKernel({0.6, 0.8}, 1.0, 0.0),
            densityKernel({0.6, 0.8}, 1.0, 1.0),
        }};

        double maximum_reach = 0.0;
        for (const DensityKernel &kernel: kernels) {
            for (u32 step = 0u; step < 96u; ++step) {
                const double angle = 2.0 * kPi * double(step) / 96.0;
                // A q-space unit direction mapped through A to the world-plane
                // offset of the support point: (x - p) = R * rot(axis) * diag(k, eta) q.
                const double qx = kernel.m_stretch * std::cos(angle);
                const double qy = kernel.m_ratio * std::sin(angle);
                const Double2 at_support{
                    (qx * kernel.m_axis.x - qy * kernel.m_axis.y) * support,
                    (qx * kernel.m_axis.y + qy * kernel.m_axis.x) * support};
                const double reach = std::sqrt(at_support.x * at_support.x + at_support.y * at_support.y);
                maximum_reach = (std::max) (maximum_reach, reach);

                // At the support, and beyond it, the weight is exactly zero. The
                // closure constant covers the ulps the transform can lose inside R.
                PPR_TEST_ASSERT(densityWeightCells(at_support, radius, kernel) == 0.0);
                PPR_TEST_ASSERT(densityWeightCells({at_support.x * 1.01, at_support.y * 1.01}, radius, kernel) == 0.0);
                PPR_TEST_ASSERT(densityWeightCells({at_support.x * 10.0, at_support.y * 10.0}, radius, kernel) == 0.0);

                // Just inside is still live: the closure is a round-off guard, not
                // a smaller support.
                if (kernel.m_stretch == 1.5 and kernel.m_ratio == 1.0) {
                    PPR_TEST_ASSERT(
                        densityWeightCells({at_support.x * 0.999, at_support.y * 0.999}, radius, kernel) > 0.0);
                }
            }
        }

        // The maximum reach is exactly 2.2889228 r, inside the 3.0625 r cull.
        PPR_TEST_ASSERT(std::abs(maximum_reach - 2.288922837966554) <= 1.0e-13);
        PPR_TEST_ASSERT(maximum_reach <= kDensityCullRadiusScale);

        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL particle_surface_support R={:.15f}r max_reach={:.15f}r cull={:.4f}r", support,
                maximum_reach, kDensityCullRadiusScale));
    };

    PPR_UNIT_TEST (particle_surface_lattice_closes_the_rest_grid) {
        // The nominal rest pool: V = 0.25 cell, spacing 0.5 cell, neutral cues.
        // Revision 5's closure numbers: the centre keeps its unchanged peak, the
        // junction and edge midpoints sit well above theta, and the nearest-
        // contributor bound keeps the whole interior closed.
        constexpr double volume = 0.25;
        constexpr double spacing = 0.5;
        constexpr double r_cells = 0.28209479177387814;
        // Logged before the assertions so a drift reports both sides.
        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL particle_surface_lattice_actual centre={:.17g}/{:.17g} "
                "junction={:.17g}/{:.17g} edge={:.17g}/{:.17g}",
                densityLatticeSample({0.0, 0.0}), 0.613510744960683, densityLatticeSample({0.25, 0.25}),
                1.359298427893234, densityLatticeSample({0.25, 0.0}), 1.227021489921365));
        const double centre = densityLatticeSample({0.0, 0.0});
        const double junction = densityLatticeSample({0.25, 0.25});
        const double edge = densityLatticeSample({0.25, 0.0});
        PPR_TEST_ASSERT(std::abs(centre - 0.613510744960683) <= 1.0e-12);
        PPR_TEST_ASSERT(std::abs(junction - 1.359298427893234) <= 1.0e-12);
        PPR_TEST_ASSERT(std::abs(edge - 1.227021489921365) <= 1.0e-12);
        PPR_TEST_ASSERT(centre > kDensityThreshold and junction > kDensityThreshold and edge > kDensityThreshold);
        PPR_TEST_ASSERT(junction > edge and edge > centre);

        // Every interior point has a nearest lattice corner within sqrt(.125), so
        // that single contributor already bounds D from below. The bound's slack
        // above theta is what keeps the interior's samples occupied with margin.
        const double nearest_offset = std::sqrt(0.125);
        const double nearest_rho = nearest_offset / (kDensitySurfaceRadiusScale * r_cells);
        const double interior_bound = kDensityAmplitude * densityRamp(std::clamp(3.0 * (1.0 - nearest_rho), 0.0, 1.0));
        PPR_TEST_ASSERT(std::abs(interior_bound - 0.339824607) <= 1.0e-9);
        PPR_TEST_ASSERT(interior_bound > kDensityThreshold);
        PPR_TEST_ASSERT(std::abs(interior_bound - kDensityThreshold - 0.1612531785) <= 1.0e-9);
        PPR_TEST_ASSERT(densityOccupancy(interior_bound) == 1.0);

        // Area mean over one lattice cell is one: V / cell area = 0.25 / 0.25.
        constexpr u32 steps = 64u;
        double total = 0.0;
        for (u32 row = 0u; row < steps; ++row) {
            for (u32 column = 0u; column < steps; ++column) {
                total += densityLatticeSample(
                    {(double(column) + 0.5) * spacing / double(steps), (double(row) + 0.5) * spacing / double(steps)});
            }
        }
        const double mean = total / double(steps * steps);
        PPR_TEST_ASSERT(std::abs(mean - volume / (spacing * spacing)) <= 1.0e-5);
        PPR_TEST_ASSERT(volume / (spacing * spacing) == 1.0);

        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL particle_surface_lattice V={:.2f} spacing={:.2f} centre={:.12f} "
                "junction={:.12f} edge={:.12f} bound={:.12f} mean={:.9f}",
                volume, spacing, centre, junction, edge, interior_bound, mean));
    };

    PPR_UNIT_TEST (particle_surface_label_selection_keeps_the_first_stable_row) {
        // Strict Less over the stable compacted order: the first minimum wins and an
        // exactly equal distance never advances to a later instance. The stored
        // depth is the minimum over ELIGIBLE quadrature points; a completely
        // excluded candidate stores nothing, which lets the next eligible label win.
        struct Candidate {
            u32 m_row{};
            u32 m_label{};
            double m_rho2{};
            bool m_eligible{true};
        };

        const auto select = [](const std::vector<Candidate> &candidates) -> std::optional<Candidate> {
            std::optional<Candidate> winner;
            for (const Candidate &candidate: candidates) {
                if (not candidate.m_eligible)
                    continue;
                if (not winner or candidate.m_rho2 < winner->m_rho2)
                    winner = candidate;
            }

            return winner;
        };

        // Nearest wins.
        const auto nearest = select({{0u, 11u, 0.60}, {1u, 12u, 0.40}, {2u, 13u, 0.80}});
        PPR_TEST_ASSERT(nearest and nearest->m_row == 1u and nearest->m_label == 12u);
        // Exact tie: the first row keeps the texel.
        const auto tied = select({{0u, 11u, 0.40}, {1u, 12u, 0.40}, {2u, 13u, 0.40}});
        PPR_TEST_ASSERT(tied and tied->m_row == 0u and tied->m_label == 11u);
        // A real quantisation tie. The 16-bit angle word spans PI (180 deg per
        // 65536 codes, kParticleOrientationAngleCount), so the metric is NOT
        // invariant under angle + 32768 (that is +90 deg): the invariance angle ->
        // angle + 65536 wraps to the same word. What the shipped pipeline does
        // provide is binary16 STORAGE: the conic record is three half coefficients,
        // so two different words whose float32 metrics differ still decode to the
        // same metric, hence to the same selection distance. The tie must keep the
        // first row, never reorder by material.
        u32 conic_tie_row = 0xffff'ffffu;
        {
            // Mirror of grid_particle_native.slang particleConicBits:
            // radians = angle*pi/65536, b = 0.3999*bias8/255,
            // k = 1 + 0.5*stretch8/255, and the inverse-metric coefficients
            // a = C^2*along + S^2*across, b = C*S*(along - across),
            // c = S^2*along + C^2*across with along = 1/k^2, across = ratio^2.
            const auto coefficients = [](const u32 angle, const u32 bias, const u32 stretch) {
                const float radians = static_cast<float>(angle) * std::numbers::pi_v<float> / 65536.0f;
                const float anisotropy = kGridParticleTangentContourBias * (static_cast<float>(bias) / 255.0f);
                const float stretch_scale = 1.0f +
                                            (kGridParticleStretchCap - 1.0f) * (static_cast<float>(stretch) / 255.0f);
                const float along = 1.0f / (stretch_scale * stretch_scale);
                const float across_ratio = (1.0f + 0.5f * anisotropy) / (1.0f - 0.5f * anisotropy);
                const float across = across_ratio * across_ratio;
                const float cosine = std::cos(radians);
                const float sine = std::sin(radians);
                return std::array<float, 3u>{
                    cosine * cosine * along + sine * sine * across, cosine * sine * (along - across),
                    sine * sine * along + cosine * cosine * across};
            };
            // The stored record (f32tof16 per coefficient) and the decoded value the
            // selection depth is actually computed from.
            const auto record = [](const std::array<float, 3u> &metric) {
                return std::array<u16, 3u>{
                    ::pP::details::encodeHalf(static_cast<double>(metric[0u])),
                    ::pP::details::encodeHalf(static_cast<double>(metric[1u])),
                    ::pP::details::encodeHalf(static_cast<double>(metric[2u]))};
            };
            // Two words one angle code apart at bias 200 / stretch 64. The float32
            // metrics differ (max |delta| ~ 5.2e-5, one angle code at this cue), but
            // every component sits >= 2.4e-4 from the nearest binary16 rounding
            // midpoint (half an ulp at these magnitudes), so both store the SAME
            // record: a genuine quantisation tie, not a repeated word.
            const auto first = coefficients(16032u, 200u, 64u);
            const auto second = coefficients(16033u, 200u, 64u);
            const float widest = std::max(
                {std::abs(first[0u] - second[0u]), std::abs(first[1u] - second[1u]), std::abs(first[2u] - second[2u])});
            PPR_TEST_ASSERT(widest > 1.0e-5f);
            const std::array<u16, 3u> first_record = record(first);
            const std::array<u16, 3u> second_record = record(second);
            PPR_TEST_ASSERT(first_record == second_record);
            // The distance the strict Less test compares, from the decoded record:
            // rho^2 = a*dx^2 + 2b*dx*dy + c*dy^2, both rows bit-identical.
            const auto rho2 = [](const std::array<u16, 3u> &stored) {
                const double a = double(halfToFloat(stored[0u]));
                const double b = double(halfToFloat(stored[1u]));
                const double c = double(halfToFloat(stored[2u]));
                return a * 0.3 * 0.3 + 2.0 * b * 0.3 * 0.2 + c * 0.2 * 0.2;
            };
            const double tied_distance = rho2(first_record);
            PPR_TEST_ASSERT(rho2(second_record) == tied_distance);
            // Equal distances: the first stable row keeps the texel.
            const auto tied_conic = select({{0u, 12u, tied_distance}, {1u, 11u, tied_distance}});
            PPR_TEST_ASSERT(tied_conic and tied_conic->m_row == 0u and tied_conic->m_label == 12u);
            conic_tie_row = tied_conic->m_row;
        }
        // An excluded first candidate cannot starve the next eligible label.
        const auto excluded = select({{0u, 11u, 0.10, false}, {1u, 12u, 0.70}});
        PPR_TEST_ASSERT(excluded and excluded->m_row == 1u and excluded->m_label == 12u);
        // No eligible candidate: the texel stores no label and no depth.
        PPR_TEST_ASSERT(not select({{0u, 11u, 0.10, false}, {1u, 12u, 0.20, false}}));

        // The stored synthetic distance is clamped below the 1.0 clear value so an
        // open support that rounds to exactly 1 stays writable under strict Less,
        // and the clamp never reorders two distinct distances.
        const double clamped = std::min(0.999999999999, kDensityDepthCeiling);
        PPR_TEST_ASSERT(clamped < 1.0);

        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL particle_surface_label_ties nearest={} tied={} conic_tie={} "
                "clamped={:.9f}",
                nearest->m_row, tied->m_row, conic_tie_row, clamped));
    };

    PPR_UNIT_TEST (particle_surface_categorical_reconstruction_never_filters_ids) {
        // Four taps of one sample: left column material 11, right two foreign
        // material 12 and air. The reconstruction must return exactly one own
        // material's attributes, never a numeric blend of 11 and 12, and must
        // renormalise by the same Z for every channel.
        const Double3 plane{0.0, 0.0, 1.0};
        const SurfaceTexel a = surfaceTexel(11u, 0.60, {0.8, 0.4, 0.2}, plane, 0.5, false);
        const SurfaceTexel b = surfaceTexel(12u, 0.90, {0.1, 0.2, 0.9}, {0.3, 0.0, 0.954}, 0.7, false);
        const SurfaceTexel air{};
        const std::array<SurfaceTexel, 4u> taps{a, b, a, air};

        // Nearest = the top-left own tap; fx = fy = .5 so the weights split.
        const auto own = densityReconstruct(taps, a, 0.5, 0.5, false);
        PPR_TEST_ASSERT(own.m_valid);
        PPR_TEST_ASSERT(own.m_material == 11u);
        // Z counts the two own taps and the air tap only: .25 + .25 + .25 = .75.
        const double z = 0.75;
        PPR_TEST_ASSERT(std::abs(own.m_density - (0.25 * 0.60 + 0.25 * 0.60) / z) <= 1.0e-15);
        // Z = U/Z / (D) is the own material's base: no foreign contribution at all.
        PPR_TEST_ASSERT(std::abs(own.m_base.x - 0.8) <= 1.0e-15);
        PPR_TEST_ASSERT(std::abs(own.m_base.y - 0.4) <= 1.0e-15);
        PPR_TEST_ASSERT(std::abs(own.m_base.z - 0.2) <= 1.0e-15);
        PPR_TEST_ASSERT(std::abs(own.m_alpha - 0.5) <= 1.0e-15);
        PPR_TEST_ASSERT(own.m_material != 12u);

        // The same taps with material 12 nearest return 12's attributes; the id
        // itself is never averaged, and a returned id is always one of the taps.
        const auto foreign = densityReconstruct(taps, b, 0.5, 0.5, false);
        PPR_TEST_ASSERT(foreign.m_valid);
        PPR_TEST_ASSERT(foreign.m_material == 12u);
        PPR_TEST_ASSERT(std::abs(foreign.m_base.z - 0.9) <= 1.0e-15);
        for (const u32 id: {own.m_material, foreign.m_material})
            PPR_TEST_ASSERT(id == 11u or id == 12u);

        // All-foreign and all-air taps: no sample exists, and the caller discards.
        const std::array<SurfaceTexel, 4u> other{b, b, b, b};
        PPR_TEST_ASSERT(not densityReconstruct(other, a, 0.25, 0.25, false).m_valid);
        const std::array<SurfaceTexel, 4u> empty{air, air, air, air};
        PPR_TEST_ASSERT(not densityReconstruct(empty, air, 0.25, 0.25, false).m_valid);

        // The analytic quotient derivative is in density-per-pixel units: a
        // one-texel linear ramp of the same label gives |dD/dx| = the step.
        const SurfaceTexel left = surfaceTexel(11u, 0.0, {0.5, 0.5, 0.5}, plane, 1.0, false);
        const SurfaceTexel right = surfaceTexel(11u, 1.0, {0.5, 0.5, 0.5}, plane, 1.0, false);
        const std::array<SurfaceTexel, 4u> ramp{left, right, left, right};
        const auto ramp_mid = densityReconstruct(ramp, left, 0.5, 0.0, false);
        PPR_TEST_ASSERT(ramp_mid.m_valid);
        PPR_TEST_ASSERT(std::abs(ramp_mid.m_density - 0.5) <= 1.0e-15);
        PPR_TEST_ASSERT(std::abs(ramp_mid.m_derivative_x - 1.0) <= 1.0e-15);
        PPR_TEST_ASSERT(std::abs(ramp_mid.m_derivative_y) <= 1.0e-15);

        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL particle_surface_reconstruction own_id={} own_D={:.9f} own_base=({:.6f},"
                "{:.6f},{:.6f}) foreign_id={} ramp_D={:.9f} ramp_dDdx={:.9f}",
                own.m_material, own.m_density, own.m_base.x, own.m_base.y, own.m_base.z, foreign.m_material,
                ramp_mid.m_density, ramp_mid.m_derivative_x));
    };

    PPR_UNIT_TEST (particle_surface_two_liquids_of_one_class_keep_one_owner_per_texel) {
        // Two OPAQUE liquid materials touching. Each texel still stores exactly one
        // label, so an interior sample reconstructs one material's attributes and a
        // border sample renormalises instead of averaging the two albedos.
        const Double3 plane{0.0, 0.0, 1.0};
        const SurfaceTexel water = surfaceTexel(11u, 1.0, {0.1, 0.3, 0.9}, plane, 1.0, false);
        const SurfaceTexel brine = surfaceTexel(12u, 1.0, {0.6, 0.6, 0.2}, plane, 1.0, false);
        const SurfaceTexel empty{};

        // Interior of water and of brine: exactly that material, full density.
        const std::array<SurfaceTexel, 4u> water_interior{water, water, water, water};
        const auto inside_water = densityReconstruct(water_interior, water, 0.3, 0.4, false);
        PPR_TEST_ASSERT(inside_water.m_valid and inside_water.m_material == 11u);
        PPR_TEST_ASSERT(std::abs(inside_water.m_density - 1.0) <= 1.0e-15);
        PPR_TEST_ASSERT(std::abs(inside_water.m_base.x - 0.1) <= 1.0e-15);
        const std::array<SurfaceTexel, 4u> brine_interior{brine, brine, brine, brine};
        const auto inside_brine = densityReconstruct(brine_interior, brine, 0.6, 0.6, false);
        PPR_TEST_ASSERT(inside_brine.m_valid and inside_brine.m_material == 12u);
        PPR_TEST_ASSERT(std::abs(inside_brine.m_base.x - 0.6) <= 1.0e-15);

        // Border at interior fractional weights: taps are {water, brine, water, air}
        // with fx = fy = 0.5, so every tap weight is 1/4 and BOTH the matching and
        // the foreign weights are nonzero; the air tap keeps its denominator weight.
        // Derived from the gather itself (`Z = sum beta*e` with e = matching or
        // empty, `U.D = sum beta*[matching]*D`, `F = U/Z`), for each side:
        //
        //   water nearest: e = {own, foreign, own, air} = {1, 0, 1, 1}
        //     D = (1/4*1 + 1/4*1) / (3/4) = 2/3,
        //     base.rgb = (1/4*albedo + 1/4*albedo) / (1/2) = albedo = (0.1, 0.3, 0.9),
        //     alpha = (1/4*1 + 1/4*1) / (1/2) = 1.
        //   brine nearest: e = {foreign, own, foreign, air} = {0, 1, 0, 1}
        //     D = (1/4*1) / (1/2) = 1/2,
        //     base.rgb = (1/4*albedo) / (1/4) = albedo = (0.6, 0.6, 0.2),
        //     alpha = (1/4*1) / (1/4) = 1.
        //
        // A foreign nonempty tap leaves numerator AND denominator (design §4.3 and
        // §9.3) while air keeps its weight, so the owner never picks up the other
        // liquid's albedo and no air slit appears, and the two sides renormalise by
        // DIFFERENT denominators (3/4 owner with two taps, 1/2 owner with one tap). A
        // shared denominator of 1 would give 1/2 and 1/4, and a filtered numerator
        // would give the blended 0.2 on both sides; both regressions fail the
        // assertions below.
        constexpr double water_density = (1.0 / 4.0 + 1.0 / 4.0) / (3.0 / 4.0);
        constexpr double brine_density = (1.0 / 4.0) / (1.0 / 2.0);
        const std::array<SurfaceTexel, 4u> border_taps{water, brine, water, empty};
        const auto water_side = densityReconstruct(border_taps, water, 0.5, 0.5, false);
        PPR_TEST_ASSERT(water_side.m_valid and water_side.m_material == 11u);
        PPR_TEST_ASSERT(std::abs(water_side.m_density - water_density) <= 1.0e-15);
        PPR_TEST_ASSERT(std::abs(water_side.m_base.x - 0.1) <= 1.0e-15);
        PPR_TEST_ASSERT(std::abs(water_side.m_base.y - 0.3) <= 1.0e-15);
        PPR_TEST_ASSERT(std::abs(water_side.m_base.z - 0.9) <= 1.0e-15);
        PPR_TEST_ASSERT(std::abs(water_side.m_alpha - 1.0) <= 1.0e-15);
        const auto brine_side = densityReconstruct(border_taps, brine, 0.5, 0.5, false);
        PPR_TEST_ASSERT(brine_side.m_valid and brine_side.m_material == 12u);
        PPR_TEST_ASSERT(std::abs(brine_side.m_density - brine_density) <= 1.0e-15);
        PPR_TEST_ASSERT(std::abs(brine_side.m_base.x - 0.6) <= 1.0e-15);
        PPR_TEST_ASSERT(std::abs(brine_side.m_base.y - 0.6) <= 1.0e-15);
        PPR_TEST_ASSERT(std::abs(brine_side.m_base.z - 0.2) <= 1.0e-15);
        PPR_TEST_ASSERT(std::abs(brine_side.m_alpha - 1.0) <= 1.0e-15);
        // Confirms the per-label denominators above: 2/3 - 1/2 = 1/6.
        PPR_TEST_ASSERT(std::abs(water_side.m_density - brine_side.m_density) > 0.1);
        // Non-categorical alternatives of the same taps, for contrast: the plain
        // filtered attribute mean every tap contributes to,
        // (0.1 + 0.6 + 0.1 + 0)/4 = 0.2, and a gather that kept the foreign tap's
        // numerator but excluded it from Z, 0.2/(3/4) = 0.2667. The categorical
        // gather returns the owner's own 0.1 and differs from both by > 0.05, which
        // is what keeps two liquids of one class visually distinct at a border.
        constexpr double filtered_base = (0.1 + 0.6 + 0.1 + 0.0) / 4.0;
        constexpr double foreign_numerator_base = filtered_base / (3.0 / 4.0);
        PPR_TEST_ASSERT(std::abs(water_side.m_base.x - filtered_base) > 0.05);
        PPR_TEST_ASSERT(std::abs(water_side.m_base.x - foreign_numerator_base) > 0.05);

        // Mixed classes: exactly one liquid class owns a sample. The same taps
        // reconstruct for the matching class and reject the other.
        const SurfaceTexel oil = surfaceTexel(13u, 1.0, {0.9, 0.7, 0.1}, plane, 0.4, true);
        const std::array<SurfaceTexel, 4u> oil_interior{oil, oil, oil, oil};
        PPR_TEST_ASSERT(densityReconstruct(oil_interior, oil, 0.5, 0.5, true).m_valid);
        PPR_TEST_ASSERT(not densityReconstruct(oil_interior, oil, 0.5, 0.5, false).m_valid);
        PPR_TEST_ASSERT(densityReconstruct(water_interior, water, 0.5, 0.5, false).m_valid);
        PPR_TEST_ASSERT(not densityReconstruct(water_interior, water, 0.5, 0.5, true).m_valid);

        // Optical alpha is the reconstructed mean, applied once: 0.4 for oil,
        // exactly 1 for an opaque liquid.
        const auto oil_sample = densityReconstruct(oil_interior, oil, 0.5, 0.5, true);
        const SurfaceComposite oil_composite = densityComposite(oil_sample, {1.0, 1.0, 1.0}, 1.0, true);
        PPR_TEST_ASSERT(oil_composite.m_visible);
        PPR_TEST_ASSERT(std::abs(oil_composite.m_alpha - 0.4) <= 1.0e-15);
        PPR_TEST_ASSERT(std::abs(oil_composite.m_premultiplied.x - 0.4) <= 1.0e-15);
        const auto water_sample = densityReconstruct(water_interior, water, 0.5, 0.5, false);
        const SurfaceComposite water_composite = densityComposite(water_sample, {1.0, 1.0, 1.0}, 1.0, false);
        PPR_TEST_ASSERT(std::abs(water_composite.m_alpha - 1.0) <= 1.0e-15);

        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL particle_surface_two_liquids water=({:.3f}) brine=({:.3f}) "
                "border_water=({:.3f},D={:.4f}) border_brine=({:.3f},D={:.4f}) oil_alpha={:.3f} "
                "filtered_would_be=({:.3f}) foreign_numerator_would_be=({:.3f})",
                inside_water.m_base.x, inside_brine.m_base.x, water_side.m_base.x, water_side.m_density,
                brine_side.m_base.x, brine_side.m_density, oil_composite.m_alpha, filtered_base,
                foreign_numerator_base));
    };

    PPR_UNIT_TEST (particle_surface_deferred_composite_premultiplies_once_and_guards_the_field) {
        // Two same-label splats with different sampled attributes: the stored Q/G
        // are weighted sums and the composite recovers weighted means, then
        // premultiplies exactly once. Nothing is shaded per splat.
        constexpr double weight_a = 0.6;
        constexpr double weight_b = 0.4;
        const Double3 base_a{0.8, 0.4, 0.2};
        const Double3 base_b{0.2, 0.4, 0.8};
        const Double3 normal_a{0.0, 0.0, 1.0};
        const Double3 normal_b{0.6, 0.0, 0.8};
        constexpr double alpha_a = 0.25;
        constexpr double alpha_b = 0.75;
        // The stored texel is the accumulation of both splats' own contributions.
        const Double3 mean_base{
            base_a.x * weight_a + base_b.x * weight_b, base_a.y * weight_a + base_b.y * weight_b,
            base_a.z * weight_a + base_b.z * weight_b};
        const Double3 mean_normal{
            normal_a.x * weight_a + normal_b.x * weight_b, normal_a.y * weight_a + normal_b.y * weight_b,
            normal_a.z * weight_a + normal_b.z * weight_b};
        const SurfaceTexel summed = surfaceTexel(
            11u, weight_a + weight_b, mean_base, mean_normal, weight_a * alpha_a + weight_b * alpha_b, true);
        const std::array<SurfaceTexel, 4u> taps{summed, summed, summed, summed};
        const auto reconstructed = densityReconstruct(taps, summed, 0.5, 0.5, true);
        PPR_TEST_ASSERT(reconstructed.m_valid);
        PPR_TEST_ASSERT(std::abs(reconstructed.m_density - 1.0) <= 1.0e-15);
        PPR_TEST_ASSERT(std::abs(reconstructed.m_base.x - 0.56) <= 1.0e-15);
        PPR_TEST_ASSERT(std::abs(reconstructed.m_base.z - 0.44) <= 1.0e-15);
        // The mean normal is renormalized, not summed raw:
        // 0.6 * 1.0 + 0.4 * 0.8 = 0.92 in z and 0.4 * 0.6 = 0.24 in x.
        const Double3 raw_normal{0.24, 0.0, 0.92};
        const double raw_length = std::sqrt(
            raw_normal.x * raw_normal.x + raw_normal.y * raw_normal.y + raw_normal.z * raw_normal.z);
        PPR_TEST_ASSERT(std::abs(reconstructed.m_normal.z - raw_normal.z / raw_length) <= 1.0e-15);
        PPR_TEST_ASSERT(std::abs(reconstructed.m_normal.x - raw_normal.x / raw_length) <= 1.0e-15);
        // Mean alpha: 0.45, applied once by the composite as C*alpha. C is the
        // union max(B, F); fractional C now comes only from the marker's own ramp
        // (the body's B is binary), and this probe pins the premultiplication
        // arithmetic for any C.
        PPR_TEST_ASSERT(std::abs(reconstructed.m_alpha - 0.45) <= 1.0e-15);
        constexpr double coverage = 0.8;
        const SurfaceComposite composite = densityComposite(reconstructed, reconstructed.m_base, coverage, true);
        PPR_TEST_ASSERT(composite.m_visible);
        PPR_TEST_ASSERT(std::abs(composite.m_alpha - 0.36) <= 1.0e-15);
        PPR_TEST_ASSERT(std::abs(composite.m_premultiplied.x - 0.56 * 0.36) <= 1.0e-15);
        // Premultiplying twice would halve it again; that is the defect this pins.
        PPR_TEST_ASSERT(std::abs(composite.m_premultiplied.x - 0.56 * 0.36 * 0.45) > 1.0e-3);

        // An opaque liquid ignores the stored alpha entirely.
        const SurfaceComposite opaque = densityComposite(reconstructed, reconstructed.m_base, coverage, false);
        PPR_TEST_ASSERT(std::abs(opaque.m_alpha - coverage) <= 1.0e-15);

        // Degenerate normal: the reconstructed vector falls back to the plane
        // normal instead of normalizing noise.
        const SurfaceTexel cancelled = surfaceTexel(11u, 1.0, {0.5, 0.5, 0.5}, {0.0, 0.0, 0.0}, 1.0, false);
        const std::array<SurfaceTexel, 4u> cancelled_taps{cancelled, cancelled, cancelled, cancelled};
        const auto cancelled_sample = densityReconstruct(cancelled_taps, cancelled, 0.5, 0.5, false);
        PPR_TEST_ASSERT(cancelled_sample.m_normal.x == 0.0 and cancelled_sample.m_normal.y == 0.0);
        PPR_TEST_ASSERT(cancelled_sample.m_normal.z == 1.0);

        // Empty-field divide guard and the negligible-coverage floor: both leave no
        // sample for the caller instead of a NaN or a defaulted visible colour.
        const SurfaceReconstruction absent{};
        PPR_TEST_ASSERT(not densityComposite(absent, {1.0, 1.0, 1.0}, 1.0, true).m_visible);
        SurfaceReconstruction tiny = reconstructed;
        tiny.m_density = kDensityEpsilon;
        PPR_TEST_ASSERT(not densityComposite(tiny, {1.0, 1.0, 1.0}, 1.0, true).m_visible);
        tiny.m_density = 2.0 * kDensityEpsilon;
        PPR_TEST_ASSERT(densityComposite(tiny, {1.0, 1.0, 1.0}, 1.0, true).m_visible);
        PPR_TEST_ASSERT(
            not densityComposite(reconstructed, reconstructed.m_base, kDensityCoverageFloor, true).m_visible);
        PPR_TEST_ASSERT(
            densityComposite(reconstructed, reconstructed.m_base, 2.0 * kDensityCoverageFloor, true).m_visible);

        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL particle_surface_deferred D={:.9f} base=({:.6f},{:.6f},{:.6f}) "
                "normal=({:.6f},{:.6f},{:.6f}) alpha={:.6f} C={:.3f} premultiplied=({:.6f},{:.6f},{:.6f})",
                reconstructed.m_density, reconstructed.m_base.x, reconstructed.m_base.y, reconstructed.m_base.z,
                reconstructed.m_normal.x, reconstructed.m_normal.y, reconstructed.m_normal.z, reconstructed.m_alpha,
                coverage, composite.m_premultiplied.x, composite.m_premultiplied.y, composite.m_premultiplied.z));
    };

    PPR_UNIT_TEST (particle_surface_reconstruction_keeps_the_true_edge_coordinate) {
        // Review S2: the reconstruction must use the ACTUAL raster sample coordinate.
        // With a 4-texel field, a sample at x = W - 0.125 uses the cell
        // floor(x - .5) = W - 1 with fx = 0.375 and the tap at x = W as AIR; a clamp to
        // the last texel centre would use fx = 0 and hold the edge at that centre's
        // value, giving zero extension on three edges and a held edge on two.
        constexpr u32 extent = 4u;
        const Double3 plane{0.0, 0.0, 1.0};
        // Only the last row/column is labelled, so the edge tap set and weights are
        // observable in the density.
        std::vector<SurfaceTexel> field(std::size_t{extent} * extent);
        const auto set = [&field, &plane](const u32 x, const u32 y) {
            field[std::size_t{y} * extent + x] = surfaceTexel(11u, 1.0, {0.5, 0.5, 0.5}, plane, 1.0, false);
        };
        const auto tap = [&](const int x, const int y) {
            if (x < 0 or y < 0 or x >= int(extent) or y >= int(extent)) {
                return SurfaceTexel{};
            }

            return field[std::size_t(y) * extent + std::size_t(x)];
        };
        const auto sample = [&](const double sx, const double sy) {
            const int nearest_x = int(std::floor(sx));
            const int nearest_y = int(std::floor(sy));
            const SurfaceTexel nearest = tap(nearest_x, nearest_y);
            const int base_x = int(std::floor(sx - 0.5));
            const int base_y = int(std::floor(sy - 0.5));
            const std::array<SurfaceTexel, 4u> taps{
                tap(base_x, base_y), tap(base_x + 1, base_y), tap(base_x, base_y + 1), tap(base_x + 1, base_y + 1)};
            return densityReconstruct(taps, nearest, sx - 0.5 - double(base_x), sy - 0.5 - double(base_y), false);
        };

        // Right edge: the labelled texel at (W-1, 1) plus air at (W, 1).
        set(extent - 1u, 1u);
        const auto right = sample(double(extent) - 0.125, 1.5);
        PPR_TEST_ASSERT(right.m_valid and right.m_material == 11u);
        // fx = 0.375, fy = 0: the in-range tap carries 0.625 of the weight and the
        // outside tap is air (weight kept, value zero).
        PPR_TEST_ASSERT(std::abs(right.m_density - 0.625) <= 1.0e-15);
        // The clamped reconstruction (fx = 0) would read 1.0 here.
        PPR_TEST_ASSERT(std::abs(right.m_density - 1.0) > 0.3);

        // Left edge: symmetric, but the outside tap is at x = -1.
        std::fill(field.begin(), field.end(), SurfaceTexel{});
        set(0u, 1u);
        const auto left = sample(0.125, 1.5);
        PPR_TEST_ASSERT(left.m_valid and std::abs(left.m_density - 0.625) <= 1.0e-15);
        PPR_TEST_ASSERT(std::abs(left.m_density - 1.0) > 0.3);

        // Bottom edge (image y runs down) and top edge.
        std::fill(field.begin(), field.end(), SurfaceTexel{});
        set(1u, extent - 1u);
        const auto bottom = sample(1.5, double(extent) - 0.125);
        PPR_TEST_ASSERT(bottom.m_valid and std::abs(bottom.m_density - 0.625) <= 1.0e-15);
        PPR_TEST_ASSERT(std::abs(bottom.m_density - 1.0) > 0.3);

        std::fill(field.begin(), field.end(), SurfaceTexel{});
        set(1u, 0u);
        const auto top = sample(1.5, 0.125);
        PPR_TEST_ASSERT(top.m_valid and std::abs(top.m_density - 0.625) <= 1.0e-15);
        PPR_TEST_ASSERT(std::abs(top.m_density - 1.0) > 0.3);

        // A sample outside the target has no nearest label, so the composite discards
        // instead of reading a clamped coordinate.
        PPR_TEST_ASSERT(not sample(double(extent), 1.5).m_valid);
        PPR_TEST_ASSERT(not sample(-0.25, 1.5).m_valid);
        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL particle_surface_edge right={:.6f} left={:.6f} bottom={:.6f} "
                "top={:.6f}",
                right.m_density, left.m_density, bottom.m_density, top.m_density));
    };

    PPR_UNIT_TEST (particle_surface_categorical_derivative_matches_a_finite_difference) {
        // Review: the ramp check had Z = 1 and Zx = Zy = 0, so it would pass if the
        // quotient correction were deleted. Use a mixed matching/foreign/air pattern
        // with nonzero Z derivatives and compare the analytic derivative of the
        // categorical density against a central finite difference of the same
        // reconstruction with the label selection held fixed.
        const Double3 plane{0.0, 0.0, 1.0};
        const SurfaceTexel own = surfaceTexel(11u, 0.7, {0.4, 0.5, 0.6}, plane, 0.8, false);
        const SurfaceTexel foreign = surfaceTexel(12u, 0.9, {0.9, 0.1, 0.2}, plane, 0.3, false);
        const std::array<SurfaceTexel, 4u> taps{own, foreign, own, SurfaceTexel{}};
        constexpr double fx = 0.3;
        constexpr double fy = 0.6;
        const auto centre = densityReconstruct(taps, own, fx, fy, false);
        PPR_TEST_ASSERT(centre.m_valid);

        // Z and its derivatives are nonzero here: one foreign tap drops weight, one
        // air tap keeps it.
        const double step = 1.0e-6;
        const auto plus_x = densityReconstruct(taps, own, fx + step, fy, false);
        const auto minus_x = densityReconstruct(taps, own, fx - step, fy, false);
        const auto plus_y = densityReconstruct(taps, own, fx, fy + step, false);
        const auto minus_y = densityReconstruct(taps, own, fx, fy - step, false);
        PPR_TEST_ASSERT(plus_x.m_valid and minus_x.m_valid and plus_y.m_valid and minus_y.m_valid);
        const double numeric_x = (plus_x.m_density - minus_x.m_density) / (2.0 * step);
        const double numeric_y = (plus_y.m_density - minus_y.m_density) / (2.0 * step);
        PPR_TEST_ASSERT(std::abs(numeric_x) > 0.1 and std::abs(numeric_y) > 1.0e-3);
        PPR_TEST_ASSERT(std::abs(centre.m_derivative_x - numeric_x) <= 1.0e-6);
        PPR_TEST_ASSERT(std::abs(centre.m_derivative_y - numeric_y) <= 1.0e-6);
        // Dropping the quotient correction (Ux.D/Z alone, -0.7/0.88 = -0.795) would
        // not match the finite difference.
        constexpr double uncorrected_x = -0.7 / 0.88;
        PPR_TEST_ASSERT(std::abs(centre.m_derivative_x - numeric_x) < std::abs(uncorrected_x - numeric_x));
        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL particle_surface_derivative dDdx={:.9f} numeric={:.9f} "
                "dDdy={:.9f} numeric={:.9f} D={:.9f}",
                centre.m_derivative_x, numeric_x, centre.m_derivative_y, numeric_y, centre.m_density));
    };

    PPR_UNIT_TEST (particle_surface_terrain_exclusion_masks_each_quadrature_point) {
        // The same authoritative predicate masks each of the four quadrature
        // positions of one texel, at every stage: the label selection skips an
        // excluded point when it takes the minimum eligible rho^2, the attribute
        // accumulation skips it when it averages w, and the composite rechecks the
        // actual scene sample position. The field itself (Q.a / G.a) is what this
        // probe exercises, so it is independent of the retired (N, D, O) channels.
        //
        // For the neutral kernel the four offsets are all equidistant from the
        // texel centre (|offset| = sqrt(2)/4 pixel in each axis pair), so every
        // position contributes the same weight w_off and the identities below are
        // exact:
        //   masked(none)          = (1/4)*4*w_off = w_off
        //   masked(one excluded)  = (1/4)*3*w_off = 0.75*w_off   (all four equal)
        //   masked(all excluded)  = 0
        // The texel-centre weight is the plateau value, not w_off; the mask applies
        // to the quadrature positions, never to an interpolated centre.
        const DensityKernel neutral = densityKernel({1.0, 0.0}, 0.0, 0.0);
        constexpr double radius = 0.28209479177387814;
        // One pixel is chosen so a quadrature offset lands inside the ramp band
        // (2/3 < rho < 1) rather than on the plateau: rho = 0.35355/0.43046 =
        // 0.8213, w = 0.3399, strictly between zero and the plateau peak 0.6135.
        constexpr double pixel = 1.0;
        const double support = kDensitySurfaceRadiusScale * radius;
        const Double2 offset = densityQuadratureOffset(0u);
        const double w_offset = densityWeight({offset.x * pixel / support, offset.y * pixel / support}, neutral);
        const double centre_weight = densityWeight({0.0, 0.0}, neutral);
        PPR_TEST_ASSERT(w_offset > 0.0 and w_offset < centre_weight);

        const auto masked = [&](const std::array<bool, 4u> &excluded) {
            double weight = 0.0;
            for (u32 corner = 0u; corner < 4u; ++corner) {
                if (excluded[corner])
                    continue;
                const Double2 sample = densityQuadratureOffset(corner);
                weight += densityWeight({sample.x * pixel / support, sample.y * pixel / support}, neutral);
            }

            return 0.25 * weight;
        };

        const std::array<bool, 4u> none{false, false, false, false};
        const std::array<bool, 4u> all{true, true, true, true};
        PPR_TEST_ASSERT(std::abs(masked(none) - w_offset) <= 1.0e-15);
        PPR_TEST_ASSERT(std::abs(masked(all)) <= 1.0e-15);

        // Every quadrant is individually maskable, and the four single-exclusion
        // results are identical: the predicate is position-independent, so no
        // quadrant is privileged and none leaks past the mask.
        for (u32 corner = 0u; corner < 4u; ++corner) {
            std::array<bool, 4u> one{false, false, false, false};
            one[corner] = true;
            const double kept = masked(one);
            PPR_TEST_ASSERT(std::abs(kept - 0.75 * w_offset) <= 1.0e-15);
            PPR_TEST_ASSERT(std::abs(kept - masked(std::array<bool, 4u>{true, false, false, false})) <= 1.0e-15);
        }

        // A fully excluded splat contributes nothing, so its label can never be
        // selected (the selection has no eligible point and discards) and its
        // density can never be accumulated.
        PPR_TEST_ASSERT(masked(all) == 0.0 and densityOccupancy(masked(all)) == 0.0);
        // Two of four excluded keeps exactly half of the unmasked weight, which is
        // the same mask applied at two positions.
        PPR_TEST_ASSERT(std::abs(masked(std::array<bool, 4u>{true, true, false, false}) - 0.5 * w_offset) <= 1.0e-15);

        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL particle_surface_terrain_mask none={:.12f} one_excluded={:.12f} "
                "all_excluded={:.12f} centre={:.12f}",
                masked(none), masked(std::array<bool, 4u>{true, false, false, false}), masked(all), centre_weight));
    };

    // ------------------------------------------------------------------
    // screen-space particle density GPU fixture
    // ------------------------------------------------------------------
    // One reusable extension of the existing GridPass device fixture: it stages
    // one liquid material, one prepared publication and one chunk, encodes the
    // real density preparation and either the real per-sample composite or the
    // direct classes alone, then reads the density target and the composite back.
    // No new harness and no visual golden: every assertion below is numeric.

    // grid_particle_density.slang's minimum-presence marker, mirrored in the units
    // the shader uses: RESOLVED RASTER SAMPLES. makePresentationGrid derives
    // resolved = ceil(window/P) content pixels and raster = ceil(resolved/2), so one
    // raster sample is two content pixels and the design contract - stated in
    // CONTENT pixels - converts by that factor alone. The winner record's
    // +/-8-sample unorm offset range and its /65535 material encoding are unchanged.
    inline constexpr double kMarkerContentPixelsPerRasterSample = 2.0;
    inline constexpr double kMarkerCoreRadiusContentPx = 1.0;
    inline constexpr double kMarkerHalfCoverageRadiusContentPx = 1.5;
    inline constexpr double kMarkerOuterRadiusContentPx = 2.0;
    // 1.0 raster sample, 0.75 and 0.5: a 2-content-pixel opaque diameter, a
    // 3-content-pixel half-coverage diameter and a 4-content-pixel outer diameter.
    inline constexpr double kMarkerRadiusPx = kMarkerOuterRadiusContentPx / kMarkerContentPixelsPerRasterSample;
    inline constexpr double kMarkerHalfCoverageRadiusPx = kMarkerHalfCoverageRadiusContentPx /
                                                          kMarkerContentPixelsPerRasterSample;
    inline constexpr double kMarkerCoreRadiusPx = kMarkerCoreRadiusContentPx / kMarkerContentPixelsPerRasterSample;
    // kParticleMarkerQuadRadius: the coverage radius plus half a raster sample's
    // diagonal, the exact bound that keeps every sample with F > 0 represented.
    inline constexpr double kMarkerQuadRadiusPx = kMarkerRadiusPx + 0.7071067811865476;
    inline constexpr double kMarkerRecordScale = 8.0;
    inline constexpr double kMarkerMaterialScale = 65535.0;

    struct DensityParticleSpec {
        double m_x_m{};
        double m_y_m{};
        double m_radius_m{0.28209479177387814};
        // The row's own material id. Every distinct id the cohort publishes gets a
        // palette entry, so a cohort can exercise several admitted materials.
        u32 m_material{1u};
        bool m_enabled{true};
    };

    // Optional authored palette override for one material id: a non-constant
    // albedo (with its alpha), a signed normal map, the repeat/period authoring and
    // the palette-driven emission the deferred response reads from the label.
    struct DensityMaterialSpec {
        u32 m_material{1u};
        u32 m_albedo_image{0xffff'ffffu};
        u32 m_normal_image{0xffff'ffffu};
        bool m_repeat{false};
        float m_texture_period_m{1.0f};
        float m_normal_scale{1.0f};
        // Authored emission per channel, as plain floats. A `float3` member would be
        // an indeterminate read when a cohort omits the initializer: Mango's own
        // camera header warns that "bare `{}` does not reliably zero simd vectors",
        // so `float3 m_emissive_rgb{}` keeps whatever occupied those bytes. The stage
        // range-checks these three channels (0..1), so an omitted initializer would
        // fail staging at random, with only a generic invalid_argument to show. Plain
        // floats default to exactly zero, so an omitted channel means authored black.
        float m_emissive_r{};
        float m_emissive_g{};
        float m_emissive_b{};
        float m_emissive_strength{};
        u32 m_color_rgba{0x0000'ffffu};
    };

    struct DensityCohortSpec {
        std::vector<DensityParticleSpec> m_particles{};
        // World metres across the raster. The whole cohorts share one cell metre.
        float m_world_extent_m{8.0f};
        u32 m_raster{256u};
        // Material 1 is optical when this is true; the explicit list below marks
        // any other material optical instead.
        bool m_optical{};
        std::vector<u32> m_optical_materials{};
        // Optional authored overrides keyed by material id, and the element images
        // they reference. Both are staged with the frame, so the splat path samples
        // a real non-constant texture through its seeded UV.
        std::vector<DensityMaterialSpec> m_materials{};
        std::vector<image::ImageAsset> m_images{};
        // Subpixel camera translation in raster pixels (+x right, +y down).
        double m_camera_offset_px{};
        // When true the draw callback encodes the direct classes without the
        // composite; used to prove routed rows emit no direct instances.
        bool m_direct_only{};
    };

    struct DensityFrameResult {
        std::vector<float> m_density{}; // Q.a per raster texel
        std::vector<u32> m_label{}; // L per raster texel
        std::vector<std::array<float, 4u>> m_base{}; // Q per raster texel
        std::vector<std::array<float, 4u>> m_normal_alpha{}; // G per raster texel
        // The SMOOTHED field the composite reconstructs, read beside the
        // accumulation field so a probe can compare before and after the filter.
        std::vector<float> m_filtered_density{};
        std::vector<u32> m_filtered_label{};
        std::vector<std::array<float, 4u>> m_filtered_base{};
        std::vector<std::array<float, 4u>> m_filtered_normal_alpha{};
        // The minimum-presence winner record per raster texel, DECODED: .xy is the
        // nearest liquid particle's centre as an offset in pixels from that texel's
        // centre, .z is its material id and .w is unused. Zero material is empty.
        std::vector<std::array<double, 4u>> m_marker{};
        std::vector<float> m_composite{}; // premultiplied RGBA per composite pixel
        u32 m_raster{};
        u32 m_phase_mask{};
        u32 m_class_mask{};
        float m_pixel_world{};
        // The smoothing scale the frame actually used: its own raster samples per
        // world native cell and the loaded halo (the shared tile's clamped tap
        // count per axis).
        float m_samples_per_cell{};
        u32 m_halo{};
    };

    [[nodiscard]] DensityFrameResult readDensityFrame(
        rhi::IDevice &device, const GridPass::ParticleSurfaceTargetsForTest &targets, rhi::ITexture &composite,
        const u32 raster) {
        DensityFrameResult result{};
        result.m_raster = raster;
        result.m_phase_mask = targets.m_phase_mask;
        result.m_class_mask = targets.m_class_mask;
        result.m_pixel_world = targets.m_pixel_world;
        result.m_samples_per_cell = targets.m_samples_per_cell;
        result.m_halo = targets.m_halo;
        PPR_TEST_ASSERT(targets.m_label != nullptr and targets.m_base != nullptr and targets.m_normal != nullptr);
        PPR_TEST_ASSERT(
            targets.m_filtered_label != nullptr and
            targets.m_filtered_base != nullptr and
            targets.m_filtered_normal != nullptr);

        const TextureReadback label = readTexture(device, *targets.m_label);
        result.m_label.resize(std::size_t{raster} * raster);
        for (u32 y = 0u; y < raster; ++y) {
            for (u32 x = 0u; x < raster; ++x) {
                const auto *const words = reinterpret_cast<const u32 *>(
                    label.m_bytes.data() + std::size_t{y} * label.m_row_pitch + std::size_t{x} * 4u);
                result.m_label[std::size_t{y} * raster + x] = *words;
            }
        }

        const auto read_half4 = [&](rhi::ITexture &texture, std::vector<std::array<float, 4u>> &into) {
            const TextureReadback readback = readTexture(device, texture);
            into.resize(std::size_t{raster} * raster);
            for (u32 y = 0u; y < raster; ++y) {
                for (u32 x = 0u; x < raster; ++x) {
                    const auto *const words = reinterpret_cast<const u16 *>(
                        readback.m_bytes.data() + std::size_t{y} * readback.m_row_pitch + std::size_t{x} * 8u);
                    for (u32 channel = 0u; channel < 4u; ++channel) {
                        into[std::size_t{y} * raster + x][channel] = halfToFloat(words[channel]);
                    }
                }
            }
        };

        read_half4(*targets.m_base, result.m_base);
        read_half4(*targets.m_normal, result.m_normal_alpha);
        result.m_density.resize(std::size_t{raster} * raster);
        for (std::size_t texel = 0u; texel < result.m_density.size(); ++texel) {
            result.m_density[texel] = result.m_base[texel][3u];
        }

        // The smoothed bundle: the same three images after both passes.
        const TextureReadback filtered_label = readTexture(device, *targets.m_filtered_label);
        result.m_filtered_label.resize(std::size_t{raster} * raster);
        for (u32 y = 0u; y < raster; ++y) {
            for (u32 x = 0u; x < raster; ++x) {
                const auto *const words = reinterpret_cast<const u32 *>(
                    filtered_label.m_bytes.data() + std::size_t{y} * filtered_label.m_row_pitch + std::size_t{x} * 4u);
                result.m_filtered_label[std::size_t{y} * raster + x] = *words;
            }
        }

        read_half4(*targets.m_filtered_base, result.m_filtered_base);
        read_half4(*targets.m_filtered_normal, result.m_filtered_normal_alpha);
        result.m_filtered_density.resize(std::size_t{raster} * raster);
        for (std::size_t texel = 0u; texel < result.m_filtered_density.size(); ++texel) {
            result.m_filtered_density[texel] = result.m_filtered_base[texel][3u];
        }

        // The marker winner record: RGBA16 unorm, 8 bytes per raster texel. Decode
        // both channels here so every probe speaks pixels and material ids instead
        // of re-deriving the shader's encoding.
        PPR_TEST_ASSERT(targets.m_marker != nullptr);
        const TextureReadback marker_readback = readTexture(device, *targets.m_marker);
        result.m_marker.resize(std::size_t{raster} * raster);
        for (u32 y = 0u; y < raster; ++y) {
            for (u32 x = 0u; x < raster; ++x) {
                const auto *const words = reinterpret_cast<const u16 *>(
                    marker_readback.m_bytes.data() +
                    std::size_t{y} * marker_readback.m_row_pitch +
                    std::size_t{x} * 8u);
                const auto value = [&words](const u32 channel) {
                    // Unorm16 -> [0,1]; the material channel's own scale is the same
                    // 65535 the shader encodes with.
                    return double(words[channel]) / 65535.0;
                };
                std::array<double, 4u> &record = result.m_marker[std::size_t{y} * raster + x];
                record[0u] = (value(0u) - 0.5) * kMarkerRecordScale;
                record[1u] = (value(1u) - 0.5) * kMarkerRecordScale;
                record[2u] = std::floor(value(2u) * kMarkerMaterialScale + 0.5);
                record[3u] = value(3u);
            }
        }

        const TextureReadback composite_readback = readTexture(device, composite);
        result.m_composite.resize(std::size_t{raster} * raster * 4u);
        for (u32 y = 0u; y < raster; ++y) {
            for (u32 x = 0u; x < raster; ++x) {
                const auto *const words = reinterpret_cast<const u16 *>(
                    composite_readback.m_bytes.data() +
                    std::size_t{y} * composite_readback.m_row_pitch +
                    std::size_t{x} * 8u);
                for (u32 channel = 0u; channel < 4u; ++channel) {
                    result.m_composite[(std::size_t{y} * raster + x) * 4u + channel] = halfToFloat(words[channel]);
                }
            }
        }

        return result;
    }

    // Runs `frames` frames of one cohort and returns each frame's readback. A
    // `hold_after_first` run advances the shared live epoch after the first
    // accepted frame so the next preparation takes the retained-row hold path.
    [[nodiscard]] std::vector<DensityFrameResult> runDensityCohort(
        const DensityCohortSpec &spec, const u32 frames, const bool hold_after_first = false) {
        std::vector<DensityFrameResult> results;
        Fixture fixture{};
        fixture.initialize();
        Renderer renderer{};
        PPR_TEST_ASSERT(not renderer.initialize(*fixture.m_rhi));
        GridPass grid{};
        PPR_TEST_ASSERT(not grid.initialize(*fixture.m_rhi, *fixture.m_shaders, std::filesystem::current_path()));
        PPR_DEFER {
            PPR_TEST_ASSERT(not renderer.waitOnHost());
            PPR_TEST_ASSERT(not grid.shutdown());
            PPR_TEST_ASSERT(not renderer.shutdown());
            fixture.shutdown();
        };

        auto &device = fixture.device();
        const u32 raster = spec.m_raster;

        // One palette entry per distinct material the cohort publishes, so a
        // cohort can stage several admitted liquids of one class or of both
        // classes. Material 1 always exists: the empty-lane case still needs a
        // publication that carries the admitted phase.
        std::vector<u32> material_ids{1u};
        for (const DensityParticleSpec &particle: spec.m_particles) {
            if (std::ranges::find(material_ids, particle.m_material) == material_ids.end())
                material_ids.push_back(particle.m_material);
        }

        const auto is_optical_material = [&spec](const u32 material) {
            if (material == 1u and spec.m_optical)
                return true;
            return std::ranges::find(spec.m_optical_materials, material) != spec.m_optical_materials.end();
        };

        GridStagedFrame stage{};
        stage.m_world_edge = WorldLightingPass::kWorldEdge;
        stage.m_cell_size_m = 1.0f;
        // A cohort that names a texture it did not stage is a fixture defect: the
        // stage's admission gate rejects it with nothing but the gate's source line.
        // Name the offending reference here instead. 0xffffffff means "no texture".
        for (const DensityMaterialSpec &material: spec.m_materials) {
            const auto staged = [&spec](
                                    const u32 index) { return index == 0xffff'ffffu or index < spec.m_images.size(); };
            PPR_TEST_ASSERT(staged(material.m_albedo_image) and staged(material.m_normal_image));

            // The same reasoning for authored emission: the gate range-checks each
            // channel to 0..1, so an out-of-range or non-finite spec is a fixture
            // defect. Print the offender with its numbers before asserting.
            const std::array<float, 3u> emissive{material.m_emissive_r, material.m_emissive_g, material.m_emissive_b};
            bool emission_in_range = true;
            for (const float channel: emissive) {
                emission_in_range = emission_in_range and
                                    std::isfinite(channel) and
                                    channel >= 0.0f and
                                    channel <= 1.0f;
            }

            if (not emission_in_range) {
                std::println(
                    std::cerr,
                    "WORLD_LIGHTING_MATERIAL density_cohort_bad_emission material={} emissive=({},{},{}) "
                    "strength={}",
                    material.m_material, emissive[0u], emissive[1u], emissive[2u], material.m_emissive_strength);
            }

            PPR_TEST_ASSERT(emission_in_range);
        }

        for (const u32 material: material_ids) {
            GridTerrainVisual visual{};
            visual.m_material_id = material;
            // Distinct authored tints per material (so a wrongly shared or
            // fallback albedo binding cannot pass a colour comparison), and an
            // authored 0x80 alpha byte on the optical materials so a reconstructed
            // optical sample is distinguishable from an opaque one.
            const u32 red = (material * 53u) & 0xffu;
            visual.m_color_rgba = (red << 24u) | (is_optical_material(material) ? 0x0000'0080u : 0x0000'00ffu);
            visual.m_ior = 1.0f;
            visual.m_opacity = 1.0f;
            visual.m_texture_period_m = 1.0f;
            visual.m_surface_flags = kGridSurfaceParticleLiquid |
                                     (is_optical_material(material) ? kGridSurfaceOpticalBlend : 0u);

            // Optional authored override: element albedo/normal descriptors,
            // repeat mode, period, normal scale, palette emission and tint.
            const auto authored = std::ranges::find(spec.m_materials, material, &DensityMaterialSpec::m_material);
            if (authored != spec.m_materials.end()) {
                visual.m_color_rgba = authored->m_color_rgba;
                visual.m_texture_index = authored->m_albedo_image;
                visual.m_normal_texture_index = authored->m_normal_image;
                visual.m_texture_period_m = authored->m_texture_period_m;
                visual.m_normal_scale = authored->m_normal_scale;
                visual.m_emissive_rgb[0u] = authored->m_emissive_r;
                visual.m_emissive_rgb[1u] = authored->m_emissive_g;
                visual.m_emissive_rgb[2u] = authored->m_emissive_b;
                visual.m_emissive_strength = authored->m_emissive_strength;
                if (authored->m_repeat) {
                    visual.m_surface_flags |= kGridSurfaceTextureRepeat;
                }
            }

            stage.m_terrain_visuals.push_back(visual);
        }
        for (const image::ImageAsset &image: spec.m_images) {
            stage.m_terrain_images.push_back(image);
        }
        // An image-bearing cohort must also stage the world background. The image
        // inventory is uploaded and sealed only by the background preparation's
        // `prepareTerrainVisuals_` budget (256 images); the particle preparation and
        // the surface binder both run that step with a zero budget and can only reuse
        // a sealed inventory, so without the background the cohort's first frame
        // fails with `operation_in_progress` before it draws anything. The four
        // layers are the pattern the palette probes in this file use: one opaque
        // white layer and three fully transparent upper layers, all on image 0.
        if (not spec.m_images.empty()) {
            stage.m_has_background = true;
            for (u32 layer = 0u; layer < 4u; ++layer) {
                stage.m_background_layers[layer] = GridBackgroundLayer{
                    .m_texture_index = 0u,
                    .m_order = layer,
                    .m_color_rgba = layer == 0u ? 0xffff'ffffu : 0xffff'ff00u,
                    .m_repeat_period_m = 8.0f,
                    .m_parallax_factor = 0.0f};
            }
        }

        stage.m_tiles.push_back(
            GridTileSubmission{.m_dirty_mask = 1u, .m_tile_range = {0, 0, 128, 128}, .m_chunk_id = 0u});

        const std::size_t count = spec.m_particles.size();
        std::vector<GridParticleMetadataRow> rows(count);
        std::vector<std::array<float, 16u>> hot(std::max<std::size_t>(count, 1u));
        std::vector<std::array<float, 6u>> cold(std::max<std::size_t>(count, 1u));
        for (std::size_t row = 0u; row < count; ++row) {
            const auto &particle = spec.m_particles[row];
            rows[row].m_id_low = static_cast<u32>(row) + 1u;
            rows[row].m_source_row = static_cast<u32>(row);
            const auto packed_temperature = lighting::packCell(static_cast<u16>(particle.m_material), 300.0);
            PPR_TEST_ASSERT(packed_temperature.has_value());
            rows[row].m_material_temperature = *packed_temperature;
            // The pack validator admits a committed row only with a finite,
            // positive presentation radius; the fixture publishes the physical
            // radius the kernel uses.
            rows[row].m_radius = static_cast<float>(particle.m_radius_m);
            // One native cell per metre; the published hot position is in native
            // cells with the shared origin of three.
            hot[row][0u] = static_cast<float>(particle.m_x_m) + 3.0f;
            hot[row][1u] = static_cast<float>(particle.m_y_m) + 3.0f;
            hot[row][15u] = particle.m_enabled ? 1.0f : 0.0f;
            cold[row][0u] = 2.0f;
            cold[row][1u] = static_cast<float>(std::numbers::pi * particle.m_radius_m * particle.m_radius_m);
            cold[row][2u] = 1.0f;
        }

        const auto metadata = prepareGridParticleMetadata(
            device, rows, count == 0u ? 0u : safe_narrowing(count), 1.0, 1u);
        PPR_TEST_ASSERT(metadata.has_value());
        GridParticleGpuView view{};
        view.m_numerical = makeBuffer(
            device, hot.size() * sizeof(std::array<float, 16u>), sizeof(std::array<float, 16u>), hot.data());
        view.m_readonly = makeBuffer(
            device, cold.size() * sizeof(std::array<float, 6u>), sizeof(std::array<float, 6u>), cold.data());
        view.m_metadata = *metadata;
        const auto live_epoch = std::make_shared<std::atomic<u64>>(1u);
        view.m_live_epoch = live_epoch;
        view.m_world = grid.activeWorldIdentity();
        view.m_origin = 3.0;
        view.m_device_epoch = 1u;
        view.m_publication = 1u;
        view.m_terrain_revision = 1u;
        view.m_joint_tick = 1u;
        stage.m_gpu_particles = view;
        // The stage's admission gate logs only its own source line, so a rejected
        // fixture is undiagnosable from the failure alone. Report the code plus the
        // whole staged inventory first, then assert.
        const std::error_code staged_frame = grid.applyStagedFrame(stage);
        if (staged_frame) {
            std::string record = std::format(
                "WORLD_LIGHTING_MATERIAL density_stage_rejected code={} category={} message={} visuals={} "
                "images={} tiles={} rows={} background={} cell_m={:.6f} edge={} boundary={}",
                staged_frame.value(), staged_frame.category().name(), staged_frame.message(),
                stage.m_terrain_visuals.size(), stage.m_terrain_images.size(), stage.m_tiles.size(), count,
                stage.m_has_background, double(stage.m_cell_size_m), stage.m_world_edge, stage.m_boundary_material_id);
            for (const GridTerrainVisual &visual: stage.m_terrain_visuals) {
                record += std::format(
                    " visual[id={} flags={} albedo={} normal={} period={:.4f} metallic={:.4f} roughness={:.4f} "
                    "opacity={:.4f} ior={:.4f} normal_scale={:.4f} refraction={:.4f} emissive=({:.4f},{:.4f},{:.4f}) "
                    "strength={:.4f} transport={}]",
                    visual.m_material_id, visual.m_surface_flags, visual.m_texture_index, visual.m_normal_texture_index,
                    double(visual.m_texture_period_m), double(visual.m_metallic), double(visual.m_roughness),
                    double(visual.m_opacity), double(visual.m_ior), double(visual.m_normal_scale),
                    double(visual.m_refraction_depth_m), double(visual.m_emissive_rgb[0u]),
                    double(visual.m_emissive_rgb[1u]), double(visual.m_emissive_rgb[2u]),
                    double(visual.m_emissive_strength), visual.m_transport_model);
            }

            for (const image::ImageAsset &image: stage.m_terrain_images) {
                record += std::format(
                    " image[extent={}x{} mips={} format={} srgb={} block={} materialized={} bytes={}]", image.m_width,
                    image.m_height, image.m_mip_count, static_cast<u32>(image.m_format), image.m_is_srgb,
                    image.m_is_block, image.m_storage.isMaterialized(), image.m_storage.getBufferData().size());
            }

            for (const GridBackgroundLayer &layer: stage.m_background_layers) {
                record += std::format(
                    " layer[order={} texture={} rgba={:#x} period={:.4f} parallax={:.4f}]", layer.m_order,
                    layer.m_texture_index, layer.m_color_rgba, double(layer.m_repeat_period_m),
                    double(layer.m_parallax_factor));
            }

            // The helper has no test-log sink of its own (the harness gives `_` to the
            // test body), so the record goes to the captured diagnostic stream.
            std::println(std::cerr, "{}", record);
        }

        PPR_TEST_ASSERT(not staged_frame);
        PPR_TEST_ASSERT(not grid.applyGpuParticleView(view));

        CameraModel model{};
        model.m_camera_mode = ECameraProjection::orthographic;
        model.m_origin = float3{0.0f, 0.0f, -0.5f};
        model.m_ortho_scale = spec.m_world_extent_m / static_cast<float>(raster);
        Camera camera{ECameraProjection::orthographic};
        camera.updateModel(
            TimeSpan{}, model, Viewport{PixelRect{0, 0, static_cast<int>(raster), static_cast<int>(raster)}});
        const float3 centre{spec.m_world_extent_m * 0.5f, spec.m_world_extent_m * 0.5f, -0.5f};
        model.m_origin = centre -
                         camera.getRight() * (spec.m_world_extent_m * 0.5f) -
                         camera.getUp() * (spec.m_world_extent_m * 0.5f);
        // A +x camera shift moves the rendered field left in the image.
        model.m_origin = model.m_origin +
                         camera.getRight() * static_cast<float>(spec.m_camera_offset_px) * model.m_ortho_scale;
        camera.updateModel(
            TimeSpan{}, model, Viewport{PixelRect{0, 0, static_cast<int>(raster), static_cast<int>(raster)}});
        auto snapshot = camera.getSnapshot();
        snapshot.m_has_camera_cut = false;

        // Production-shaped scene target for the composite: 4x RGBA16F colour
        // with a 1x resolve, plus a 4x D32Float depth attachment. The composite
        // entry is per-sample (SV_SampleIndex / EvaluateAttributeAtSample) and the
        // production contract is the main scene's 4x + depth signature, so a 1x
        // depth-less target cannot express it. The accumulation keeps its own
        // explicit 1-sample, depth-less signature inside the prepare callback.
        const auto make_color_target = [&](const u32 sample_count, const char *const label) {
            rhi::TextureDesc description{};
            description.type = sample_count > 1u ? rhi::TextureType::Texture2DMS : rhi::TextureType::Texture2D;
            description.size = {.width = raster, .height = raster, .depth = 1u};
            description.arrayLength = 1u;
            description.mipCount = 1u;
            description.format = rhi::Format::RGBA16Float;
            description.sampleCount = sample_count;
            description.memoryType = rhi::MemoryType::DeviceLocal;
            description.usage = static_cast<rhi::TextureUsage>(
                static_cast<u32>(rhi::TextureUsage::RenderTarget) | static_cast<u32>(rhi::TextureUsage::CopySource));
            description.defaultState = rhi::ResourceState::RenderTarget;
            description.label = label;
            rhi::ComPtr<rhi::ITexture> texture{};
            PPR_TEST_ASSERT(not make_error_code(device.createTexture(description, nullptr, texture.writeRef())));
            return texture;
        };

        rhi::ComPtr<rhi::ITexture> scene_color = make_color_target(4u, "grid particle density scene colour 4x");
        rhi::ComPtr<rhi::ITexture> scene_resolve = make_color_target(1u, "grid particle density scene resolve 1x");

        rhi::TextureDesc depth_description{};
        depth_description.type = rhi::TextureType::Texture2DMS;
        depth_description.size = {.width = raster, .height = raster, .depth = 1u};
        depth_description.arrayLength = 1u;
        depth_description.mipCount = 1u;
        depth_description.format = rhi::Format::D32Float;
        depth_description.sampleCount = 4u;
        depth_description.memoryType = rhi::MemoryType::DeviceLocal;
        depth_description.usage = rhi::TextureUsage::DepthStencil;
        depth_description.defaultState = rhi::ResourceState::DepthWrite;
        depth_description.label = "grid particle density scene depth 4x";
        rhi::ComPtr<rhi::ITexture> scene_depth{};
        PPR_TEST_ASSERT(not make_error_code(device.createTexture(depth_description, nullptr, scene_depth.writeRef())));

        // Pin the target shapes the fixture claims to exercise.
        PPR_TEST_ASSERT(scene_color->getDesc().sampleCount == 4u);
        PPR_TEST_ASSERT(scene_resolve->getDesc().sampleCount == 1u);
        PPR_TEST_ASSERT(scene_depth->getDesc().sampleCount == 4u);
        PPR_TEST_ASSERT(scene_depth->getDesc().format == rhi::Format::D32Float);

        const rhi::ComPtr<rhi::ITextureView> scene_color_view = scene_color->getDefaultView();
        const rhi::ComPtr<rhi::ITextureView> scene_resolve_view = scene_resolve->getDefaultView();
        const rhi::ComPtr<rhi::ITextureView> scene_depth_view = scene_depth->getDefaultView();
        PPR_TEST_ASSERT(
            scene_color_view.get() != nullptr and
            scene_resolve_view.get() != nullptr and
            scene_depth_view.get() != nullptr);

        const auto viewport = rhi::Viewport::fromSize(static_cast<float>(raster), static_cast<float>(raster));
        const auto scissor = rhi::ScissorRect::fromSize(raster, raster);

        for (u32 frame = 0u; frame < frames; ++frame) {
            if (hold_after_first and frame == 1u) {
                // The solver bumped its live epoch: the retained publication is
                // no longer current, so this frame must present its own rows.
                live_epoch->fetch_add(1u, std::memory_order_release);
            }

            PPR_TEST_ASSERT(not grid.update(TimeSpan{}, snapshot, false));
            const auto prepare = [&](const PreRenderContext &context) -> std::error_code {
                PPR_RETURN_ERROR_ON_FAIL(GridPass, grid.prepareBackgroundInputs(context, snapshot, viewport, scissor));
                PPR_RETURN_ERROR_ON_FAIL(GridPass, grid.prepareBackground(context, snapshot, viewport, scissor));
                PPR_RETURN_ERROR_ON_FAIL(GridPass, grid.prepareGpuParticles(context));
                return grid.prepareParticleSurface(context, snapshot, viewport, scissor);
            };
            const auto draw = [&](const DrawContext &context) -> std::error_code {
                if (spec.m_direct_only) {
                    return grid.renderDirectParticleClassesForTest(context);
                }

                // Class order is the contract: opaque first, optical second, each
                // reconstructing the same label and rejecting the other class.
                if (grid.particleSurfaceClassAdmitted(EGridParticleDrawClass::opaque)) {
                    PPR_RETURN_ERROR_ON_FAIL(
                        GridPass, grid.renderParticleSurface(context, EGridParticleDrawClass::opaque));
                }

                if (grid.particleSurfaceClassAdmitted(EGridParticleDrawClass::optical)) {
                    PPR_RETURN_ERROR_ON_FAIL(
                        GridPass, grid.renderParticleSurface(context, EGridParticleDrawClass::optical));
                }

                return {};
            };
            DrawSubmission submission{"grid particle density fixture", DrawCallback{draw}};
            submission.m_viewport = viewport;
            submission.m_scissor = scissor;

            rhi::RenderPassColorAttachment color_attachment{};
            color_attachment.view = scene_color_view.get();
            color_attachment.resolveTarget = scene_resolve_view.get();
            color_attachment.loadOp = rhi::LoadOp::Clear;
            color_attachment.storeOp = rhi::StoreOp::Store;
            for (float &component: color_attachment.clearValue)
                component = 0.0f;

            rhi::RenderPassDepthStencilAttachment depth_attachment{};
            depth_attachment.view = scene_depth_view.get();
            depth_attachment.depthLoadOp = rhi::LoadOp::Clear;
            depth_attachment.depthStoreOp = rhi::StoreOp::Store;
            depth_attachment.depthClearValue = 1.0f;

            PPR_TEST_ASSERT(not renderer.render(
                string_literal{std::in_place, "grid particle density fixture"},
                rhi::RenderPassDesc{
                    .colorAttachments = &color_attachment,
                    .colorAttachmentCount = 1u,
                    .depthStencilAttachment = &depth_attachment,
                },
                {submission}, PreRenderCallback{prepare}));
            grid.notifyFrameSubmitted(renderer.lastSubmittedFenceValue());
            PPR_TEST_ASSERT(not renderer.waitOnHost());
            // The resolve target is the 1x image the pass writes at end-of-pass;
            // the readback samples that resolved texel, never the 4x surface.
            results.push_back(readDensityFrame(device, grid.particleSurfaceTargetsForTest(), *scene_resolve, raster));
        }

        return results;
    }

    // ------------------------------------------------------------------
    // seeded element sampling and per-texel oracles
    // ------------------------------------------------------------------
    // Independent restatements of the two specifications the splat path consumes:
    // the stable-identity 16-bit UV offset (design §2.5) and the authored-sRGB
    // palette tint (grid_tiles.slang diagnosticTint). They let a GPU probe predict
    // which texel a particle's own patch selects without reading shader state.
    [[nodiscard]] u32 particleSeedCpu(const u32 id_low, const u32 id_high) noexcept {
        const auto mix = [](u32 value) {
            value ^= value >> 16u;
            value *= 0x7feb352du;
            value ^= value >> 15u;
            value *= 0x846ca68bu;
            value ^= value >> 16u;
            return value;
        };
        return mix(id_low ^ mix(id_high ^ 0x9e3779b9u));
    }

    [[nodiscard]] Double2 particleUvOffsetCpu(const u32 seed) noexcept {
        return {(double(seed & 0xffffu) + 0.5) / 65536.0, (double(seed >> 16u) + 0.5) / 65536.0};
    }

    [[nodiscard]] Double3 diagnosticTintCpu(const u32 rgba) noexcept {
        const auto channel = [rgba](const u32 shift) {
            const u32 byte = (rgba >> shift) & 0xffu;
            const double linear = double(byte) / 255.0;
            return byte == 255u ? 1.0 : (linear <= 0.04045 ? linear / 12.92 : std::pow((linear + 0.055) / 1.055, 2.4));
        };
        return {channel(24u), channel(16u), channel(8u)};
    }

    // A 4x4 element whose rgb AND alpha vary per texel: a world-UV regression, a
    // wrong seed, or a mis-placed texture gradient changes the sample.
    struct PatternImage {
        std::array<std::array<u8, 4u>, 16u> m_texels{};
    };

    [[nodiscard]] PatternImage makeAlbedoPattern() noexcept {
        PatternImage pattern{};
        for (u32 y = 0u; y < 4u; ++y) {
            for (u32 x = 0u; x < 4u; ++x) {
                const u32 index = y * 4u + x;
                pattern.m_texels[index] = {
                    static_cast<u8>(32u + x * 60u), static_cast<u8>(220u - y * 56u),
                    static_cast<u8>(20u + ((x + y) & 3u) * 70u), static_cast<u8>(90u + ((x * 3u + y) & 3u) * 50u)};
            }
        }

        return pattern;
    }

    [[nodiscard]] PatternImage makeNormalPattern() noexcept {
        PatternImage pattern{};
        for (u32 y = 0u; y < 4u; ++y) {
            for (u32 x = 0u; x < 4u; ++x) {
                const double nx = double(x) / 3.0 - 0.5;
                const double ny = double(y) / 3.0 - 0.5;
                const auto encode = [](const double value) {
                    return static_cast<u8>(std::lround(std::clamp(value * 0.5 + 0.5, 0.0, 1.0) * 255.0));
                };
                pattern.m_texels[y * 4u + x] = {encode(nx), encode(ny), encode(0.8660254037844386), u8{255}};
            }
        }

        return pattern;
    }

    [[nodiscard]] image::ImageAsset patternAsset(const PatternImage &pattern) noexcept {
        constexpr u32 size = 4u;
        Array<std::byte> pixels(size * size * 4u);
        for (u32 texel = 0u; texel < size * size; ++texel) {
            for (u32 channel = 0u; channel < 4u; ++channel) {
                pixels[texel * 4u + channel] = std::byte{pattern.m_texels[texel][channel]};
            }
        }

        image::ImageAsset image{};
        image.m_width = size;
        image.m_height = size;
        // The stage's admission gate requires a LINEAR normal map (a tangent-space
        // RGB vector is not colour), and it reads the asset's own format rather than
        // trusting a palette flag. State it explicitly instead of inheriting the
        // default: a decoded or authored asset that arrives as rgba8_srgb must not be
        // able to reach the gate as a silently different enumerator.
        image.m_format = image::ENativeImageFormat::rgba8_linear;
        image.m_is_srgb = false;
        image.m_storage = mem::SharedBuffer::clone(
            mem::SharedBufferView{reinterpret_cast<const std::byte *>(pixels.data()), pixels.size()});
        image.m_subresources.push_back(
            image::ImageSubresource{
                .m_view = image.m_storage, .m_row_pitch = size * 4u, .m_slice_pitch = pixels.size()});
        PPR_TEST_ASSERT(
            not image::generateMipChain(image, image::MipGenDesc{.m_full_chain = true, .m_normal_map = false}));
        return image;
    }

    // Linear/Wrap bilinear fetch of mip 0, byte/255, as the element sampler returns
    // it at LOD 0 (the seeded gradients select mip 0 for these periods).
    [[nodiscard]] std::array<double, 4u> patternBilinear(const PatternImage &pattern, const Double2 uv) noexcept {
        constexpr double size = 4.0;
        const double x = uv.x * size - 0.5;
        const double y = uv.y * size - 0.5;
        const double x0 = std::floor(x);
        const double y0 = std::floor(y);
        const double fx = x - x0;
        const double fy = y - y0;
        const auto texel = [&pattern](const int ix, const int iy) {
            const int wx = ((ix % 4) + 4) % 4;
            const int wy = ((iy % 4) + 4) % 4;
            const auto &bytes = pattern.m_texels[std::size_t(wy) * 4u + std::size_t(wx)];
            return std::array<double, 4u>{
                double(bytes[0u]) / 255.0, double(bytes[1u]) / 255.0, double(bytes[2u]) / 255.0,
                double(bytes[3u]) / 255.0};
        };
        const auto a = texel(int(x0), int(y0));
        const auto b = texel(int(x0) + 1, int(y0));
        const auto c = texel(int(x0), int(y0) + 1);
        const auto d = texel(int(x0) + 1, int(y0) + 1);
        std::array<double, 4u> result{};
        for (u32 channel = 0u; channel < 4u; ++channel) {
            result[channel] = (a[channel] * (1.0 - fx) + b[channel] * fx) * (1.0 - fy) +
                              (c[channel] * (1.0 - fx) + d[channel] * fx) * fy;
        }

        return result;
    }

    // The particle's own element albedo at one world-plane point: the seeded,
    // particle-attached `VisibleUvContext` of design §2.6, evaluated in this CPU
    // mirror. `local` is the world-plane offset from the particle centre.
    [[nodiscard]] std::array<double, 4u> seededAlbedoCpu(
        const PatternImage &pattern, const u32 seed, const Double2 local, const double period) noexcept {
        const Double2 offset = particleUvOffsetCpu(seed);
        return patternBilinear(pattern, {offset.x + local.x / period, offset.y + local.y / period});
    }

    // One texel that disagrees with the oracle, for the failure log.
    struct SurfaceTexelMismatch {
        u32 m_x{};
        u32 m_y{};
        u32 m_label{};
        double m_measured{};
        double m_weight{};
        double m_min_rho{};
    };

    // ------------------------------------------------------------------
    // rasterised splat quad
    // ------------------------------------------------------------------
    // The shipped splat vertex places the quad at `particle +/- extent` in world
    // metres with `extent = radius*R_surface_scale*conic_support + pixel_world`
    // (particleVertexCommon) and carries the same world offset in `local_m`; the
    // fragment evaluates its four quadrature points from the INTERPOLATED
    // `local_m`. A rasteriser snaps vertex positions to its 8-bit sub-pixel grid
    // (1/256 pixel), so the quad's rasterised span differs from the ideal
    // `2*extent/pixel` by up to 1/256 pixel and the interpolated offset is scaled
    // by `ideal span / snapped span` about the snapped centre. At the isolated
    // droplet's 4.84 px support that scale is 1.7e-4, which the rim's
    // |d ln w / d rho| ~ 20-100 turns into a 0.3-1 % weight difference: the
    // selection and the accumulation both see the scaled `local_m`, so the FIELD
    // is self-consistent and only an ideal-geometry oracle disagrees. The
    // per-texel oracle must therefore snap the same two edges. This is
    // rasterisation of the shipped quad, not the kernel: no constant, support,
    // tolerance or reconstruction rule changes.
    struct RasterisedQuad {
        // Interpolated local_m of one axis: value = m_origin + pixel * m_step.
        // m_step is the shader's ddx/ddy magnitude (metres per raster pixel).
        Double2 m_origin{};
        Double2 m_step{};
    };

    [[nodiscard]] RasterisedQuad rasterisedQuad(
        const Double2 particle_px, const double extent_m, const double pixel_m) noexcept {
        // D3D12 and Vulkan both guarantee at least 8 bits of sub-pixel precision
        // for 1x rasterisation, and the observed read-back of the isolated
        // droplet resolves the snapping as nearest (a floor convention would move
        // every rim weight the other way). A tie at exactly half a grid step is
        // 1/512 pixel, below the 2e-3 weight bound.
        constexpr double kSubPixelGrid = 256.0;
        RasterisedQuad quad{};
        const auto axis = [&](const double centre_px, const bool world_up, double &origin, double &step) {
            const double extent_px = extent_m / pixel_m;
            const double first = std::round((centre_px - extent_px) * kSubPixelGrid) / kSubPixelGrid;
            const double last = std::round((centre_px + extent_px) * kSubPixelGrid) / kSubPixelGrid;
            const double span = last - first;
            // A positive radius cannot degenerate; keep the division finite rather
            // than asserting on a device-derived radius.
            if (not (span > 1.0e-9)) {
                origin = -extent_m;
                step = pixel_m;
                return;
            }

            const double magnitude = 2.0 * extent_m / span;
            // +x runs with the window column, so the first (left) vertex carries
            // -extent; world Y runs against the window row, so the first (top)
            // vertex carries +extent. local(u) = first_value + (u - first)*m.
            if (world_up) {
                origin = extent_m + first * magnitude;
                step = -magnitude;
            } else {
                origin = -extent_m - first * magnitude;
                step = magnitude;
            }
        };
        axis(particle_px.x, false, quad.m_origin.x, quad.m_step.x);
        axis(particle_px.y, true, quad.m_origin.y, quad.m_step.y);
        return quad;
    }

    // The interpolated `local_m` a fragment sees at one texel centre: the rasterised
    // quad's own offset (see RasterisedQuad). Used by the weight oracle and by the
    // seeded attribute UV, because the accumulation samples the element texture at
    // THIS offset (`particleUvContext(input.uv_seed, input.local_m, ...)`), not at the
    // ideal `world - particle.position`.
    [[nodiscard]] Double2 rasterisedLocal(const RasterisedQuad &quad, const Double2 texel_centre_px) noexcept {
        return {
            quad.m_origin.x + texel_centre_px.x * quad.m_step.x, quad.m_origin.y + texel_centre_px.y * quad.m_step.y};
    }

    // The accumulation's per-texel weight exactly as the shipped pipeline produces
    // it: the average of the four (+/-1/4 pixel) kernel points around the texel's
    // interpolated local_m, with the quad extent, quadrature offsets and kernel the
    // shader uses. `texel_centre_px` is (x + .5, y + .5).
    struct RasterisedWeight {
        double m_weight{};
        double m_min_rho{};
    };

    [[nodiscard]] RasterisedWeight rasterisedQuadratureWeight(
        const RasterisedQuad &quad, const Double2 texel_centre_px, const double radius_cells,
        const DensityKernel kernel) noexcept {
        const double support = kDensitySurfaceRadiusScale * radius_cells;
        const Double2 local = rasterisedLocal(quad, texel_centre_px);
        RasterisedWeight result{};
        result.m_min_rho = 2.0;
        double weight = 0.0;
        for (u32 corner = 0u; corner < 4u; ++corner) {
            const Double2 offset = densityQuadratureOffset(corner);
            const Double2 sample{
                (local.x + offset.x * quad.m_step.x) / support, (local.y + offset.y * quad.m_step.y) / support};
            result.m_min_rho = std::min(result.m_min_rho, densityRho(sample, kernel));
            weight += densityWeight(sample, kernel);
        }

        result.m_weight = 0.25 * weight;
        return result;
    }

    // Absolute floor of the joint invariant, derived rather than chosen. The oracle
    // and the shader derive their offsets from different precisions: the packed
    // particle position is float, the vertex forms `world - particle.position` in
    // float, and the oracle uses double. The worst case is the coarse cohort, whose
    // particle sits at ~39 m: a float ulp there is 3.8e-6 m, so the offset error is
    // ~4e-6 m, the rho error ~4.4e-6 at R = 0.9156 m, and with
    // |dw/drho| <= 3 * 1.5 * amplitude = 2.76 the weight error is ~1.2e-5 (about
    // 6.1e-6 at the span64 droplet, whose coordinates are ~5 m). 3e-5 is at least
    // 1.7x that worst-case bound, 0.6 % of the isolated peak (0.6135) and two orders
    // below the 1/255 compositing floor, so no texel that could be drawn is excluded;
    // the relative 2e-3 comparison still applies to every materially positive texel.
    // The rasteriser's sub-pixel snapping is MODELLED by RasterisedQuad above, so this
    // floor carries only the packed-position precision, not a rasterisation allowance.
    inline constexpr double kSurfaceTexelWeightFloor = 3.0e-5;

    // Joint label/density invariant of one ISOLATED particle (review S1): a texel
    // whose oracle four-point weight is materially positive must carry that
    // particle's label AND store that weight; a texel storing density must not be
    // empty. An integral comparison alone can pass while the composite ignores
    // unlabelled density, so this counts mismatching texels for the caller to assert
    // as zero, and optionally records the first offenders for the log. The oracle's
    // quad is the rasterised one (see RasterisedQuad): the vertex extent is the
    // shipped `R + pixel` and the fragment sees the interpolated local_m.
    [[nodiscard]] u32 surfaceTexelMismatches(
        const DensityFrameResult &frame, const DensityKernel kernel, const double radius_cells, const double pixel,
        const double particle_px_x, const double particle_px_y, const u32 material, const double relative_tolerance,
        std::vector<SurfaceTexelMismatch> *const offenders = nullptr) {
        const double support = kDensitySurfaceRadiusScale * radius_cells;
        const RasterisedQuad quad = rasterisedQuad({particle_px_x, particle_px_y}, support + pixel, pixel);
        u32 mismatches = 0u;
        for (u32 y = 0u; y < frame.m_raster; ++y) {
            for (u32 x = 0u; x < frame.m_raster; ++x) {
                const std::size_t index = std::size_t{y} * frame.m_raster + x;
                const RasterisedWeight oracle = rasterisedQuadratureWeight(
                    quad, {double(x) + 0.5, double(y) + 0.5}, radius_cells, kernel);
                const double weight = oracle.m_weight;
                const double measured = double(frame.m_density[index]);
                const u32 label = frame.m_label[index];
                bool mismatched = false;
                if (weight > kSurfaceTexelWeightFloor) {
                    mismatched = label != material or
                                 std::abs(measured - weight) > relative_tolerance * weight + kSurfaceTexelWeightFloor;
                } else if (measured > kSurfaceTexelWeightFloor) {
                    mismatched = true;
                }

                if (mismatched) {
                    ++mismatches;
                    if (offenders != nullptr and offenders->size() < 8u) {
                        offenders->push_back({x, y, label, measured, weight, oracle.m_min_rho});
                    }
                }
            }
        }

        return mismatches;
    }

    [[nodiscard]] std::string describeMismatches(const std::vector<SurfaceTexelMismatch> &offenders) {
        std::string text;
        for (const SurfaceTexelMismatch &offender: offenders) {
            text += std::format(
                " ({},{})L={} qa={:.7f} w={:.7f} rho={:.5f}", offender.m_x, offender.m_y, offender.m_label,
                offender.m_measured, offender.m_weight, offender.m_min_rho);
        }

        return text;
    }

    // The same shape for the pre-formatted attribute offenders of the border probe.
    [[nodiscard]] std::string describeAttributeOffenders(const std::vector<std::string> &offenders) {
        std::string text;
        for (const std::string &offender: offenders) {
            text += offender;
        }

        return text;
    }

    // ------------------------------------------------------------------
    // screen-space particle density GPU probes
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (particle_density_empty_lane_clears_and_one_splat_integrates_to_its_area) {
        // Empty admitted lane: the publication still carries the liquid material,
        // so the lane is admitted, but its only row is disabled and the cull count
        // is zero. The accumulation must clear to exact zero and the composite must
        // contribute nothing, never last frame's density.
        DensityCohortSpec empty{};
        empty.m_particles.push_back(DensityParticleSpec{.m_enabled = false});
        const auto empty_frames = runDensityCohort(empty, 1u);
        PPR_TEST_ASSERT(empty_frames.size() == 1u);
        PPR_TEST_ASSERT(empty_frames[0].m_phase_mask == 1u);
        PPR_TEST_ASSERT(empty_frames[0].m_class_mask == (1u << static_cast<u32>(EGridParticleDrawClass::opaque)));
        for (const float density: empty_frames[0].m_density)
            PPR_TEST_ASSERT(density == 0.0f);
        for (u32 pixel = 0u; pixel < empty_frames[0].m_raster * empty_frames[0].m_raster; ++pixel) {
            PPR_TEST_ASSERT(empty_frames[0].m_composite[pixel * 4u + 3u] == 0.0f);
        }

        // One enabled neutral splat at the raster centre. Its discrete integral must
        // equal the independent four-point quadrature prediction for this pixel
        // scale, and the continuous reference area V to that prediction's own
        // tolerance -- not by forcing discrete equality at an arbitrary zoom.
        constexpr double radius_m = 0.28209479177387814;
        constexpr double reference_area = kPi * radius_m * radius_m;
        DensityCohortSpec single{};
        single.m_particles.push_back(DensityParticleSpec{.m_x_m = 4.0, .m_y_m = 4.0, .m_radius_m = radius_m});
        const auto single_frames = runDensityCohort(single, 1u);
        PPR_TEST_ASSERT(single_frames.size() == 1u);
        const auto &frame = single_frames[0];
        const double pixel_world = 8.0 / 256.0;
        PPR_TEST_ASSERT(std::abs(frame.m_pixel_world - static_cast<float>(pixel_world)) <= 1.0e-7f);

        double integral = 0.0;
        for (const float density: frame.m_density)
            integral += static_cast<double>(density) * pixel_world * pixel_world;
        const DensityKernel neutral = densityKernel({1.0, 0.0}, 0.0, 0.0);
        const double predicted = densityQuadratureIntegral(neutral, radius_m, pixel_world);
        PPR_TEST_ASSERT(std::abs(integral - predicted) <= std::max(0.01 * predicted, 1.0e-4));
        PPR_TEST_ASSERT(std::abs(integral - reference_area) <= 0.01 * reference_area);

        // Composite: revision 5's isolated threshold crossing is rho = 0.88106863 of
        // the shared support, so the centre pixel is saturated and a pixel far
        // outside the support keeps the cleared target.
        const auto alpha_at = [&](const u32 x, const u32 y) {
            return frame.m_composite[(std::size_t{y} * frame.m_raster + x) * 4u + 3u];
        };
        PPR_TEST_ASSERT(alpha_at(128u, 128u) >= 0.99f);
        PPR_TEST_ASSERT(alpha_at(128u + 40u, 128u) == 0.0f);
        PPR_TEST_ASSERT(alpha_at(8u, 8u) == 0.0f);
        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL particle_density_gpu field integral={:.9f} predicted={:.9f} "
                "reference={:.9f} pixel_world={:.6f}",
                integral, predicted, reference_area, pixel_world));
    };

    PPR_UNIT_TEST (particle_density_lattice_saturates_and_holds_area_under_subpixel_translation) {
        // 4x4 rest lattice: V = 0.25 m^2, spacing sqrt(V) = 0.5 m, neutral cues.
        // Linear accumulation makes the field integral n * V regardless of overlap,
        // and the four-particle junction plus every centre lies above the contour.
        DensityCohortSpec lattice{};
        constexpr double spacing = 0.5;
        constexpr double radius_m = 0.28209479177387814;
        for (u32 row = 0u; row < 4u; ++row) {
            for (u32 column = 0u; column < 4u; ++column) {
                lattice.m_particles.push_back(
                    DensityParticleSpec{
                        .m_x_m = 3.25 + double(column) * spacing,
                        .m_y_m = 3.25 + double(row) * spacing,
                        .m_radius_m = radius_m});
            }
        }

        const auto base = runDensityCohort(lattice, 1u);
        PPR_TEST_ASSERT(base.size() == 1u);
        const auto &frame = base[0];
        const double pixel_world = 8.0 / 256.0;
        double integral = 0.0;
        for (const float density: frame.m_density)
            integral += static_cast<double>(density) * pixel_world * pixel_world;
        const double expected = 16.0 * (kPi * radius_m * radius_m);
        PPR_TEST_ASSERT(std::abs(integral - expected) <= 0.02 * expected);

        // World-to-pixel mapping: +x right, +Y up so image y runs down.
        const auto pixel_x = [&](const double world_x) { return static_cast<u32>(std::floor(world_x / pixel_world)); };
        const auto pixel_y = [&](const double world_y) {
            return static_cast<u32>(std::floor((8.0 - world_y) / pixel_world));
        };
        const auto alpha_at = [&](const u32 x, const u32 y) {
            return frame.m_composite[(std::size_t{y} * frame.m_raster + x) * 4u + 3u];
        };

        // Saturated interior: a particle centre and the four-particle junction.
        PPR_TEST_ASSERT(alpha_at(pixel_x(3.25), pixel_y(3.25)) >= 0.99f);
        PPR_TEST_ASSERT(alpha_at(pixel_x(3.5), pixel_y(3.5)) >= 0.99f);
        // Outside every support: the free surface above the lattice and the corner.
        PPR_TEST_ASSERT(alpha_at(pixel_x(4.0), pixel_y(4.0 + 3.0 * 2.0 * radius_m)) == 0.0f);
        PPR_TEST_ASSERT(alpha_at(2u, 2u) == 0.0f);

        // Subpixel camera translation of one quarter pixel. The integral is a
        // property of the field, so it is preserved; the field itself must move by
        // the translated distance rather than snapping to the pixel grid.
        DensityCohortSpec shifted = lattice;
        shifted.m_camera_offset_px = 0.25;
        const auto subpixel = runDensityCohort(shifted, 1u);
        PPR_TEST_ASSERT(subpixel.size() == 1u);
        double shifted_integral = 0.0;
        for (const float density: subpixel[0].m_density)
            shifted_integral += static_cast<double>(density) * pixel_world * pixel_world;
        PPR_TEST_ASSERT(std::abs(shifted_integral - integral) <= 0.02 * integral);

        const auto centroid_x = [&](const DensityFrameResult &result) {
            double weighted = 0.0;
            double total = 0.0;
            for (u32 y = 0u; y < result.m_raster; ++y) {
                for (u32 x = 0u; x < result.m_raster; ++x) {
                    const double density = static_cast<double>(result.m_density[std::size_t{y} * result.m_raster + x]);
                    weighted += density * (static_cast<double>(x) + 0.5);
                    total += density;
                }
            }

            return total > 0.0 ? weighted / total : 0.0;
        };
        const double moved = centroid_x(subpixel[0]) - centroid_x(frame);
        PPR_TEST_ASSERT(std::abs(moved + 0.25) <= 0.15);
        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL particle_density_lattice integral={:.9f} expected={:.9f} "
                "shifted_integral={:.9f} centroid_move_px={:.6f}",
                integral, expected, shifted_integral, moved));
    };

    PPR_UNIT_TEST (particle_density_span_and_droplet_cohorts_keep_the_reference_area) {
        // Span 64 (64 native cells across 720 px, reproduced at a 256-px raster):
        // one isolated droplet, one sparse pair whose supports do not meet, and a
        // two-cluster neck joined by one bridging particle.
        constexpr double radius_m = 0.28209479177387814;
        constexpr double span64_pixel = 1.0 / (720.0 / 64.0);
        DensityCohortSpec span64{};
        span64.m_world_extent_m = static_cast<float>(span64_pixel * 256.0);
        // Pixel-corner aligned so the independent quadrature prediction and the
        // rasterised field share one quadrature phase.
        const double droplet_x = span64_pixel * 56.0;
        const double droplet_y = span64_pixel * 45.0;
        span64.m_particles.push_back(
            DensityParticleSpec{.m_x_m = droplet_x, .m_y_m = droplet_y, .m_radius_m = radius_m});
        span64.m_particles.push_back(DensityParticleSpec{.m_x_m = 12.0, .m_y_m = 4.0, .m_radius_m = radius_m});
        span64.m_particles.push_back(DensityParticleSpec{.m_x_m = 13.5, .m_y_m = 4.0, .m_radius_m = radius_m});
        for (u32 index = 0u; index < 4u; ++index) {
            const double dx = index < 2u ? -0.45 : 0.45;
            const double dy = (index % 2u) == 0u ? -0.25 : 0.25;
            span64.m_particles.push_back(
                DensityParticleSpec{.m_x_m = 18.0 + dx, .m_y_m = 4.0 + dy, .m_radius_m = radius_m});
        }
        span64.m_particles.push_back(DensityParticleSpec{.m_x_m = 18.0, .m_y_m = 4.0, .m_radius_m = radius_m});

        const auto sparse_frames = runDensityCohort(span64, 1u);
        PPR_TEST_ASSERT(sparse_frames.size() == 1u);
        const auto &frame = sparse_frames[0];
        const auto density_at = [&](const double world_x, const double world_y) {
            const auto x = static_cast<u32>(std::floor(world_x / span64_pixel));
            const auto y = static_cast<u32>(std::floor((double(span64.m_world_extent_m) - world_y) / span64_pixel));
            return frame.m_density[std::size_t{y} * frame.m_raster + x];
        };
        const auto integral_in = [&](const double x0, const double x1, const double y0, const double y1) {
            double total = 0.0;
            const auto first_x = static_cast<u32>(std::floor(x0 / span64_pixel));
            const auto last_x = static_cast<u32>(std::floor(x1 / span64_pixel));
            const auto first_y = static_cast<u32>(std::floor((double(span64.m_world_extent_m) - y1) / span64_pixel));
            const auto last_y = static_cast<u32>(std::floor((double(span64.m_world_extent_m) - y0) / span64_pixel));
            for (u32 y = first_y; y < last_y; ++y) {
                for (u32 x = first_x; x < last_x; ++x) {
                    total += static_cast<double>(frame.m_density[std::size_t{y} * frame.m_raster + x]);
                }
            }

            return total * span64_pixel * span64_pixel;
        };

        // The isolated droplet's integral against the specified operator. Here
        // R/pixel = 1.5259486*0.282095/0.088889 = 4.84 density pixels and the
        // four-point quadrature error is 0.011 %, so the cohort is expected within
        // 0.02 % of its reference area.
        const DensityKernel neutral = densityKernel({1.0, 0.0}, 0.0, 0.0);
        const double droplet_particle_x = droplet_x / span64_pixel;
        const double droplet_particle_y = droplet_y / span64_pixel;
        const double predicted = densityFieldIntegral(
            neutral, radius_m, span64_pixel, frame.m_raster, droplet_particle_x, droplet_particle_y, 1.0);
        const double droplet = integral_in(droplet_x - 2.0, droplet_x + 2.0, droplet_y - 2.0, droplet_y + 2.0);
        // Same structural equality as the coarse cohort; the 2e-3 bound is the FP16
        // rounding of the contributing texels plus float32 arithmetic.
        PPR_TEST_ASSERT(droplet <= predicted * (1.0 + 1.0e-3));
        PPR_TEST_ASSERT(droplet >= predicted * (1.0 - 2.0e-3));
        // Joint label/density agreement texel by texel (review S1): the integral alone
        // can pass with density in texels the composite ignores. This invariant is a
        // SINGLE-particle oracle - the rest of this cohort is 7 m away and its density
        // is correctly "not this particle's weight" - so it runs on an isolated copy of
        // the same droplet at the same geometry, period and seed row.
        DensityCohortSpec isolated_droplet{};
        isolated_droplet.m_world_extent_m = span64.m_world_extent_m;
        isolated_droplet.m_particles.push_back(
            DensityParticleSpec{.m_x_m = droplet_x, .m_y_m = droplet_y, .m_radius_m = radius_m});
        const auto isolated_frames = runDensityCohort(isolated_droplet, 1u);
        PPR_TEST_ASSERT(isolated_frames.size() == 1u);
        std::vector<SurfaceTexelMismatch> droplet_offenders;
        // The droplet's raster position is (56, 45); image y runs down, so its raster y
        // is 256 - 45.
        const u32 droplet_mismatches = surfaceTexelMismatches(
            isolated_frames[0], neutral, radius_m, span64_pixel, droplet_particle_x, 256.0 - droplet_particle_y, 1u,
            2.0e-3, &droplet_offenders);
        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL particle_surface_texel_invariant span64 mismatches={} offenders={}",
                droplet_mismatches, describeMismatches(droplet_offenders)));
        PPR_TEST_ASSERT(droplet_mismatches == 0u);

        // Sparse supports do not meet: the midpoint keeps exactly zero density.
        PPR_TEST_ASSERT(density_at(12.75, 4.0) == 0.0f);
        PPR_TEST_ASSERT(integral_in(11.0, 12.8, 3.0, 5.0) > 0.0);
        PPR_TEST_ASSERT(integral_in(12.8, 14.5, 3.0, 5.0) > 0.0);

        // Neck: the bridge particle carries density across the cluster gap.
        PPR_TEST_ASSERT(density_at(18.0, 4.0) > static_cast<float>(kDensityThreshold));
        PPR_TEST_ASSERT(density_at(17.55, 4.0) > 0.0f);
        PPR_TEST_ASSERT(density_at(18.45, 4.0) > 0.0f);

        // Half-coverage width at theta against the analytic drawn contour radius
        // rho = 0.881068630339572 of the shared surface support.
        const double expected_half_width_px = 0.881068630339572 * kDensitySurfaceRadiusScale * radius_m / span64_pixel;
        u32 crossing = 0u;
        for (u32 step = 0u; step < 64u; ++step) {
            const double world_x = droplet_x + (double(step) + 0.5) * span64_pixel;
            if (density_at(world_x, droplet_y) >= static_cast<float>(kDensityThreshold)) {
                crossing = step + 1u;
            }
        }

        PPR_TEST_ASSERT(std::abs(double(crossing) - expected_half_width_px) <= 0.6);

        // Span 220: the same view contract at 220 cells across 720 px. A droplet
        // whose half-contour diameter is at least two scene pixels must not vanish;
        // its integral still matches the discrete prediction within the coarser
        // quadrature tolerance.
        constexpr double span220_pixel = 1.0 / (720.0 / 220.0);
        DensityCohortSpec span220{};
        span220.m_world_extent_m = static_cast<float>(span220_pixel * 256.0);
        const double coarse_x = span220_pixel * 128.0;
        span220.m_particles.push_back(DensityParticleSpec{.m_x_m = coarse_x, .m_y_m = coarse_x, .m_radius_m = 0.6});
        const auto coarse_frames = runDensityCohort(span220, 1u);
        PPR_TEST_ASSERT(coarse_frames.size() == 1u);
        const auto &coarse = coarse_frames[0];
        float peak = 0.0f;
        for (const float density: coarse.m_density)
            peak = std::max(peak, density);
        PPR_TEST_ASSERT(peak >= static_cast<float>(kDensityThreshold));
        u32 covered = 0u;
        for (u32 pixel = 0u; pixel < coarse.m_raster * coarse.m_raster; ++pixel) {
            if (coarse.m_composite[pixel * 4u + 3u] > 0.0f)
                ++covered;
        }

        PPR_TEST_ASSERT(covered >= 8u);
        // Revision 5 shrank the support by 24 %, so at this zoom the kernel spans
        // R/pixel = 1.5259486*0.6/0.3055556 = 3.00 density pixels and the particle
        // sits on a pixel corner. The specified operator at this cohort's raster
        // phase and quad clip is 1.128821114, only 0.19 % below V: the four-point
        // quadrature is not a several-percent effect at this zoom.
        const double coarse_particle_px = coarse_x / span220_pixel;
        const double coarse_ideal = densityFieldIntegral(
            neutral, 0.6, span220_pixel, coarse.m_raster, coarse_particle_px, coarse_particle_px, 1.0);
        const double coarse_volume = kPi * 0.6 * 0.6;
        double coarse_integral = 0.0;
        for (const float density: coarse.m_density)
            coarse_integral += static_cast<double>(density) * span220_pixel * span220_pixel;
        std::vector<SurfaceTexelMismatch> coarse_offenders;
        const u32 coarse_mismatches = surfaceTexelMismatches(
            coarse, neutral, 0.6, span220_pixel, coarse_particle_px, coarse_particle_px, 1u, 2.0e-3, &coarse_offenders);
        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL particle_density_span droplet={:.9f} predicted={:.9f} "
                "half_width_px={} expected_px={:.4f} coarse_peak={:.4f} coarse_covered={} "
                "coarse_integral={:.9f} coarse_operator={:.9f} coarse_volume={:.9f} "
                "coarse_vs_operator={:.4f} coarse_texel_mismatches={} offenders={}",
                droplet, predicted, crossing, expected_half_width_px, peak, covered, coarse_integral, coarse_ideal,
                coarse_volume, coarse_integral / coarse_ideal - 1.0, coarse_mismatches,
                describeMismatches(coarse_offenders)));
        // The field must EQUAL the specified operator, not merely sit near it. That is
        // structural, not a fitted tolerance: every shaded texel with a positive
        // averaged weight stores that weight (the accumulation rejects only a FOREIGN
        // label) and the quad of half-extent R + 1 px shades every texel whose weight
        // can be positive (reach R + sqrt(2)/4 px). The 2e-3 bound is FP16 per-texel
        // rounding (<= 5e-4 relative) plus float32 shader arithmetic.
        PPR_TEST_ASSERT(coarse_integral <= coarse_ideal * (1.0 + 1.0e-3));
        PPR_TEST_ASSERT(coarse_integral >= coarse_ideal * (1.0 - 2.0e-3));
        // Joint label/density agreement texel by texel, including every
        // boundary-straddling texel (review S1): positive weight must be labelled by
        // this material and must store that weight; a storing texel may not be empty.
        PPR_TEST_ASSERT(coarse_mismatches == 0u);
        // Cohort-specific one-sided statement (not a general theorem: a positive
        // quadrature may over- or underestimate the continuum). For this phase the
        // measured operator is below V, so the read-back must be too.
        PPR_TEST_ASSERT(coarse_ideal < coarse_volume);
        PPR_TEST_ASSERT(coarse_integral <= coarse_volume);
    };

    PPR_UNIT_TEST (particle_surface_material_border_matches_the_quadrature_oracle) {
        // Two same-class liquids touching at a categorical material border, each with
        // a NON-CONSTANT seeded albedo (rgb and alpha vary per texel). Foreign-label
        // rejection diverges per texel at that border, which is exactly where ddx/ddy
        // must already be coherent: a derivative taken after the rejection yields
        // garbage quadrature offsets and the border weights stop matching this oracle.
        // Three subpixel camera translations move the border across the raster.
        const PatternImage albedo = makeAlbedoPattern();
        const PatternImage normal_map = makeNormalPattern();
        constexpr double radius_m = 0.28209479177387814;
        constexpr double period_m = 4.0;
        constexpr u32 material_a = 11u;
        constexpr u32 material_b = 12u;
        constexpr u32 tint_a = 0xc080'40ffu;
        constexpr u32 tint_b = 0x40c0'80ffu;
        const DensityKernel neutral = densityKernel({1.0, 0.0}, 0.0, 0.0);
        const double support = kDensitySurfaceRadiusScale * radius_m;

        u32 checked_texels = 0u;
        u32 border_texels = 0u;
        std::vector<std::string> attribute_offenders;
        for (const double offset_px: {0.0, 0.25, 0.5}) {
            DensityCohortSpec cohort{};
            cohort.m_images = {patternAsset(albedo), patternAsset(normal_map)};
            cohort.m_materials = {
                DensityMaterialSpec{
                    .m_material = material_a,
                    .m_albedo_image = 0u,
                    .m_normal_image = 1u,
                    .m_repeat = true,
                    .m_texture_period_m = static_cast<float>(period_m),
                    .m_color_rgba = tint_a},
                DensityMaterialSpec{
                    .m_material = material_b,
                    .m_albedo_image = 0u,
                    .m_normal_image = 1u,
                    .m_repeat = true,
                    .m_texture_period_m = static_cast<float>(period_m),
                    .m_color_rgba = tint_b},
            };
            cohort.m_particles = {
                DensityParticleSpec{.m_x_m = 4.0, .m_y_m = 4.0, .m_radius_m = radius_m, .m_material = material_a},
                DensityParticleSpec{.m_x_m = 4.0, .m_y_m = 4.5, .m_radius_m = radius_m, .m_material = material_b},
            };
            cohort.m_camera_offset_px = offset_px;
            const auto frames = runDensityCohort(cohort, 1u);
            PPR_TEST_ASSERT(frames.size() == 1u);
            const auto &frame = frames[0];
            const double pixel = 8.0 / 256.0;
            const u32 seeds[2u] = {particleSeedCpu(1u, 0u), particleSeedCpu(2u, 0u)};
            const Double2 centres[2u] = {{4.0, 4.0}, {4.0, 4.5}};
            const u32 tints[2u] = {tint_a, tint_b};
            // Each particle's own rasterised quad: the shipped vertex extent is
            // `R + one density pixel`, the +x camera shift moves the particle left in
            // the image by that many pixels, and the rasteriser's 1/256-pixel vertex
            // snapping scales the interpolated local_m (see RasterisedQuad). The two
            // rows sit half a metre apart, so their quads snap differently.
            RasterisedQuad quads[2u]{};
            for (u32 particle = 0u; particle < 2u; ++particle) {
                const Double2 particle_px{centres[particle].x / pixel - offset_px, (8.0 - centres[particle].y) / pixel};
                quads[particle] = rasterisedQuad(particle_px, support + pixel, pixel);
            }

            for (u32 y = 0u; y < frame.m_raster; ++y) {
                for (u32 x = 0u; x < frame.m_raster; ++x) {
                    const std::size_t index = std::size_t{y} * frame.m_raster + x;
                    const Double2 texel{double(x) + 0.5, double(y) + 0.5};
                    const double weights[2u] = {
                        rasterisedQuadratureWeight(quads[0u], texel, radius_m, neutral).m_weight,
                        rasterisedQuadratureWeight(quads[1u], texel, radius_m, neutral).m_weight};
                    const u32 label = frame.m_label[index];
                    const double measured = double(frame.m_density[index]);
                    ++checked_texels;
                    // Positive weight must be labelled by one of the two materials and
                    // must store THAT material's weight (review S1's joint invariant:
                    // the integral alone can pass with density in texels the composite
                    // ignores). The oracle's four points are the shader's: the four
                    // +/-1/4-pixel offsets of the (rasterised) quad, averaged by 1/4,
                    // with only the winner's own row accumulating (a foreign label
                    // leaves the numerator and, in the composite gather, the
                    // denominator too).
                    if (weights[0u] > 1.0e-6 or weights[1u] > 1.0e-6) {
                        PPR_TEST_ASSERT(label == material_a or label == material_b);
                        const double expected_weight = label == material_a ? weights[0u] : weights[1u];
                        PPR_TEST_ASSERT(std::abs(measured - expected_weight) <= 2.0e-3 * expected_weight + 1.0e-6);
                        ++border_texels;
                    } else {
                        PPR_TEST_ASSERT(measured <= 1.0e-6);
                    }

                    if (label != material_a and label != material_b) {
                        continue;
                    }

                    // The winning material's own seeded patch, evaluated at the SAME
                    // interpolated local_m the accumulation used: `UV = offset(seed) +
                    // local_m / period` with local_m from the rasterised quad, not the
                    // ideal `world - particle.position`. A world-UV regression, a wrong
                    // seed or the wrong patch frame samples a different texel here.
                    // (The gradient the shader passes to SampleGrad is step/period; at
                    // these periods and quad scales that selects mip 0, which is what
                    // seededAlbedoCpu's bilinear mirrors.)
                    const u32 winner = label == material_a ? 0u : 1u;
                    const Double2 local = rasterisedLocal(quads[winner], texel);
                    const auto sampled = seededAlbedoCpu(albedo, seeds[winner], local, period_m);
                    const Double3 tint = diagnosticTintCpu(tints[winner]);
                    // The reconstructed base is Q.rgb/Q.a, so it exists only where the
                    // accumulation stored density; the composite itself discards
                    // D <= kDensityEpsilon before recovering attributes. The minimum
                    // stored weight of this cohort is ~1e-5, where the binary16 ratio
                    // keeps <= 0.6 % error, so no texel the renderer would draw is lost.
                    const double denominator = std::max(measured, 1.0e-6);
                    const std::array<double, 3u> expected{
                        tint.x * sampled[0u], tint.y * sampled[1u], tint.z * sampled[2u]};
                    const std::array<double, 3u> actual{
                        double(frame.m_base[index][0u]) / denominator, double(frame.m_base[index][1u]) / denominator,
                        double(frame.m_base[index][2u]) / denominator};
                    // Offender log: the measured channels beside BOTH models (the
                    // rasterised local_m above and the ideal world offset), so a
                    // failure names the residual instead of a bare boolean.
                    const Double2 ideal{
                        (double(x) + 0.5 + offset_px) * pixel - centres[winner].x,
                        8.0 - (double(y) + 0.5) * pixel - centres[winner].y};
                    const auto ideal_sample = seededAlbedoCpu(albedo, seeds[winner], ideal, period_m);
                    const std::array<double, 3u> ideal_expected{
                        tint.x * ideal_sample[0u], tint.y * ideal_sample[1u], tint.z * ideal_sample[2u]};
                    if (measured > kDensityEpsilon) {
                        for (u32 channel = 0u; channel < 3u; ++channel) {
                            if (std::abs(actual[channel] - expected[channel]) > 0.02 and
                                attribute_offenders.size() < 4u) {
                                attribute_offenders.push_back(
                                    std::format(
                                        " ({},{})o={:.2f}L={} D={:.6f} ch={} actual={:.5f} snapped={:.5f} "
                                        "ideal={:.5f} local=({:.5f},{:.5f}) local_ideal=({:.5f},{:.5f})",
                                        x, y, offset_px, label, measured, channel, actual[channel], expected[channel],
                                        ideal_expected[channel], local.x, local.y, ideal.x, ideal.y));
                            }

                            PPR_TEST_ASSERT(std::abs(actual[channel] - expected[channel]) <= 0.02);
                        }

                        // Authored alpha is applied once: an opaque liquid's stored
                        // G.a/Q.a is exactly one (visibleSurfaceAlpha).
                        PPR_TEST_ASSERT(std::abs(double(frame.m_normal_alpha[index][3u]) / denominator - 1.0) <= 0.02);
                    }

                    // Signed varying normal: the same seeded context and the same
                    // rasterised local_m sample the element's normal map, decoded
                    // (value*2-1 with the xy near-zero snap), scaled and normalized,
                    // and accumulated weighted.
                    if (measured > 0.05) {
                        const auto encoded = seededAlbedoCpu(normal_map, seeds[winner], local, period_m);
                        std::array<double, 3u> decoded{
                            2.0 * encoded[0u] - 1.0, 2.0 * encoded[1u] - 1.0, 2.0 * encoded[2u] - 1.0};
                        for (u32 axis = 0u; axis < 2u; ++axis) {
                            if (std::abs(decoded[axis]) <= 1.0 / 255.0 + 1.0e-6) {
                                decoded[axis] = 0.0;
                            }
                        }

                        const double length = std::sqrt(
                            decoded[0u] * decoded[0u] + decoded[1u] * decoded[1u] + decoded[2u] * decoded[2u]);
                        PPR_TEST_ASSERT(length > 1.0e-4 and decoded[2u] > 0.0);
                        for (u32 axis = 0u; axis < 3u; ++axis) {
                            const double measured_normal = double(frame.m_normal_alpha[index][axis]) / denominator;
                            PPR_TEST_ASSERT(std::abs(measured_normal - decoded[axis] / length) <= 0.02);
                        }
                    }
                }
            }
        }

        PPR_TEST_ASSERT(border_texels > 0u);
        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL particle_surface_border texels={} weighted={} offsets=3 offenders={}",
                checked_texels, border_texels, describeAttributeOffenders(attribute_offenders)));
    };

    PPR_UNIT_TEST (particle_surface_equal_distance_competition_keeps_the_first_row) {
        // Two DIFFERENT materials at the same position with identical radius and cue:
        // every selection distance is exactly equal, so strict Less must keep the first
        // row in stable compacted order for every texel, and the foreign label must
        // reject the second material's weight (one owner, not a sum).
        constexpr double radius_m = 0.28209479177387814;
        constexpr u32 material_a = 11u;
        constexpr u32 material_b = 12u;
        DensityCohortSpec cohort{};
        for (const u32 material: {material_a, material_b}) {
            cohort.m_particles.push_back(
                DensityParticleSpec{.m_x_m = 4.0, .m_y_m = 4.0, .m_radius_m = radius_m, .m_material = material});
        }

        const auto frames = runDensityCohort(cohort, 1u);
        PPR_TEST_ASSERT(frames.size() == 1u);
        const auto &frame = frames[0];
        const DensityKernel neutral = densityKernel({1.0, 0.0}, 0.0, 0.0);
        const double support = kDensitySurfaceRadiusScale * radius_m;
        const double pixel = 8.0 / 256.0;
        // Both rows share the cached cue and radius, so they share one quad.
        const RasterisedQuad quad = rasterisedQuad({4.0 / pixel, 4.0 / pixel}, support + pixel, pixel);
        u32 labelled = 0u;
        for (u32 y = 0u; y < frame.m_raster; ++y) {
            for (u32 x = 0u; x < frame.m_raster; ++x) {
                const std::size_t index = std::size_t{y} * frame.m_raster + x;
                const u32 label = frame.m_label[index];
                if (label == 0u) {
                    PPR_TEST_ASSERT(double(frame.m_density[index]) <= 1.0e-6);
                    continue;
                }

                // First row wins every tied texel, and only that row's weight is stored.
                PPR_TEST_ASSERT(label == material_a);
                ++labelled;
                // The rasterised quad: the shipped vertex extent is R + one density
                // pixel, and the rasteriser's 1/256-pixel vertex snapping scales the
                // interpolated local_m (see RasterisedQuad).
                const RasterisedWeight oracle = rasterisedQuadratureWeight(
                    quad, {double(x) + 0.5, double(y) + 0.5}, radius_m, neutral);
                const double weight = oracle.m_weight;
                PPR_TEST_ASSERT(std::abs(double(frame.m_density[index]) - weight) <= 2.0e-3 * weight + 1.0e-6);
            }
        }

        PPR_TEST_ASSERT(labelled > 0u);
        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL particle_surface_equal_distance labelled={} owner={}", labelled, material_a));
    };

    PPR_UNIT_TEST (particle_surface_authored_emission_is_added_once) {
        // Two otherwise identical opaque-liquid cohorts differ only in the palette's
        // authored emission. The liquid response adds `1 - exp(-emissive*min(strength,80))`
        // ONCE per surviving sample and never multiplies it by D, so the resolved green
        // difference at the covered centre is exactly that term (C = 1 there), not
        // term*D and not term applied per splat.
        constexpr double radius_m = 0.28209479177387814;
        const auto run = [&](const float3 emissive, const float strength) {
            DensityCohortSpec cohort{};
            cohort.m_materials = {DensityMaterialSpec{
                .m_material = 1u,
                .m_emissive_r = emissive.x,
                .m_emissive_g = emissive.y,
                .m_emissive_b = emissive.z,
                .m_emissive_strength = strength,
                .m_color_rgba = 0x0000'0000u,
            }};
            cohort.m_particles.push_back(DensityParticleSpec{.m_x_m = 4.0, .m_y_m = 4.0, .m_radius_m = radius_m});
            return runDensityCohort(cohort, 1u);
        };

        const auto dark = run(float3{0.0f, 0.0f, 0.0f}, 0.0f);
        const auto lit = run(float3{0.0f, 1.0f, 0.0f}, 1.0f);
        PPR_TEST_ASSERT(dark.size() == 1u and lit.size() == 1u);
        constexpr u32 raster = 256u;
        const std::size_t centre = std::size_t{128u} * raster + 128u;
        const double expected = 1.0 - std::exp(-1.0);
        const double measured = double(lit[0].m_composite[centre * 4u + 1u]) -
                                double(dark[0].m_composite[centre * 4u + 1u]);
        PPR_TEST_ASSERT(std::abs(measured - expected) <= 0.02);
        // Multiply-by-density would give term*D ~= 0.632*0.6135 and would fail here.
        PPR_TEST_ASSERT(std::abs(measured - expected * 0.613510744960683) > 0.1);
        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL particle_surface_emission delta={:.6f} expected={:.6f} D={:.6f}", measured,
                expected, 0.613510744960683));
    };

    PPR_UNIT_TEST (particle_surface_composite_uses_four_distinct_scene_samples) {
        // The composite is sample-rate: at each of the four standard 4x sample positions
        // it reconstructs the field at THAT position and decides binary occupancy there.
        // Production has no hardware resolve at all: the scene attachment is a 4x
        // Texture2DMS without a resolve target and the reconstruction pass loads the
        // individual samples (hable_temporal.slang g_scene.Load(pixel, sample)). This
        // probe's own 1x resolve attachment is only the readback path, so the resolved
        // alpha of a boundary pixel is the fraction of its four covered samples - a
        // multiple of 1/4 for this opaque liquid - and differs from the pixel-centre
        // decision.
        constexpr double radius_m = 0.28209479177387814;
        constexpr double spacing = 0.5;
        DensityCohortSpec cohort{};
        for (u32 row = 0u; row < 2u; ++row) {
            for (u32 column = 0u; column < 2u; ++column) {
                cohort.m_particles.push_back(
                    DensityParticleSpec{
                        .m_x_m = 4.0 + double(column) * spacing,
                        .m_y_m = 4.0 + double(row) * spacing,
                        .m_radius_m = radius_m});
            }
        }

        const auto frames = runDensityCohort(cohort, 1u);
        PPR_TEST_ASSERT(frames.size() == 1u);
        const auto &frame = frames[0];
        // The composite reconstructs the SMOOTHED field, so the model below must
        // read the same bundle: labels, density and attributes all come from the
        // vertical pass's output, never from the accumulation targets.
        const auto texel = [&](const int ix, const int iy) {
            SurfaceTexel value{};
            if (ix < 0 or iy < 0 or ix >= int(frame.m_raster) or iy >= int(frame.m_raster)) {
                return value;
            }

            const std::size_t index = std::size_t(iy) * frame.m_raster + std::size_t(ix);
            value.m_label = frame.m_filtered_label[index];
            value.m_base = Double3{
                double(frame.m_filtered_base[index][0u]), double(frame.m_filtered_base[index][1u]),
                double(frame.m_filtered_base[index][2u])};
            value.m_density = double(frame.m_filtered_base[index][3u]);
            value.m_alpha = double(frame.m_filtered_normal_alpha[index][3u]);
            return value;
        };
        const auto coverage_at = [&](const double sx, const double sy) -> double {
            const int nearest_x = int(std::floor(sx));
            const int nearest_y = int(std::floor(sy));
            const SurfaceTexel nearest = texel(nearest_x, nearest_y);
            if (nearest.m_label == 0u) {
                return 0.0;
            }

            const int base_x = int(std::floor(sx - 0.5));
            const int base_y = int(std::floor(sy - 0.5));
            const double fx = sx - 0.5 - double(base_x);
            const double fy = sy - 0.5 - double(base_y);
            const std::array<SurfaceTexel, 4u> taps{
                texel(base_x, base_y), texel(base_x + 1, base_y), texel(base_x, base_y + 1),
                texel(base_x + 1, base_y + 1)};
            const auto reconstructed = densityReconstruct(taps, nearest, fx, fy, false);
            if (not reconstructed.m_valid) {
                return 0.0;
            }

            return densityOccupancy(reconstructed.m_density);
        };
        constexpr std::array<std::array<double, 2u>, 4u> sample_positions{{
            {0.375, 0.125},
            {0.875, 0.375},
            {0.125, 0.625},
            {0.625, 0.875},
        }};

        double best_gap = 0.0;
        double best_resolved = 0.0;
        double best_mean = 0.0;
        double best_centre = 0.0;
        for (u32 y = 1u; y + 1u < frame.m_raster; ++y) {
            for (u32 x = 1u; x + 1u < frame.m_raster; ++x) {
                const std::size_t index = std::size_t{y} * frame.m_raster + x;
                const double resolved = double(frame.m_composite[index * 4u + 3u]);
                if (resolved <= 0.0 or resolved >= 1.0) {
                    continue;
                }

                double mean = 0.0;
                for (const auto &offset: sample_positions) {
                    mean += coverage_at(double(x) + offset[0u], double(y) + offset[1u]);
                }

                mean *= 0.25;
                const double centre = coverage_at(double(x) + 0.5, double(y) + 0.5);
                const double gap = std::abs(mean - centre);
                if (gap > best_gap) {
                    best_gap = gap;
                    best_resolved = resolved;
                    best_mean = mean;
                    best_centre = centre;
                }
            }
        }

        // The cohort must actually straddle the occupancy boundary within its pixels,
        // otherwise the four-sample test would be vacuous: a binary sample pattern and a
        // pixel-centre decision differ by at least one whole sample.
        PPR_TEST_ASSERT(best_gap >= 0.25 - 1.0e-6);
        // A resolved pixel's alpha is the covered-sample FRACTION times the material's
        // opacity (1 here), so the boundary pixel takes 1, 2 or 3 of its 4 samples and
        // its alpha is exactly that count / 4 -- no intermediate ramp value exists.
        const double covered_samples = best_mean * 4.0;
        PPR_TEST_ASSERT(std::abs(covered_samples - std::round(covered_samples)) <= 1.0e-6);
        PPR_TEST_ASSERT(std::round(covered_samples) >= 1.0 and std::round(covered_samples) <= 3.0);
        PPR_TEST_ASSERT(std::abs(best_resolved - best_mean) <= 0.03);
        PPR_TEST_ASSERT(std::abs(best_resolved - best_centre) >= 0.02);
        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL particle_surface_sample_rate resolved={:.6f} mean4={:.6f} "
                "centre={:.6f} gap={:.6f} covered_samples={:.0f}",
                best_resolved, best_mean, best_centre, best_gap, covered_samples));
    };

    PPR_UNIT_TEST (particle_surface_two_materials_of_one_class_hold_one_label_per_texel) {
        // Two OPAQUE liquids of one publication touching: material 11 at y = 4.0 and
        // material 12 at y = 4.5. The selection stores exactly one material per
        // texel and the accumulation only sums the winning label's own rows, so no
        // texel mixes the two albedos and no texel is unlabelled inside the union.
        constexpr double radius_m = 0.28209479177387814;
        DensityCohortSpec cohort{};
        cohort.m_particles.push_back(
            DensityParticleSpec{.m_x_m = 4.0, .m_y_m = 4.0, .m_radius_m = radius_m, .m_material = 11u});
        cohort.m_particles.push_back(
            DensityParticleSpec{.m_x_m = 4.0, .m_y_m = 4.5, .m_radius_m = radius_m, .m_material = 12u});
        const auto frames = runDensityCohort(cohort, 1u);
        PPR_TEST_ASSERT(frames.size() == 1u);
        const auto &frame = frames[0];
        constexpr u32 raster = 256u;
        const double pixel_world = 8.0 / 256.0;
        const auto pixel_x = [&](const double world_x) { return static_cast<u32>(std::floor(world_x / pixel_world)); };
        const auto pixel_y = [&](const double world_y) {
            return static_cast<u32>(std::floor((8.0 - world_y) / pixel_world));
        };
        const auto at = [&](const u32 x, const u32 y) { return std::size_t{y} * raster + x; };

        // Exactly one admitted class, and only the two published materials appear.
        PPR_TEST_ASSERT(frame.m_phase_mask == 1u);
        PPR_TEST_ASSERT(frame.m_class_mask == (1u << static_cast<u32>(EGridParticleDrawClass::opaque)));
        for (const u32 label: frame.m_label) {
            PPR_TEST_ASSERT(label == 0u or label == 11u or label == 12u);
        }

        // Each particle's own centre is owned by its own material, and the stored
        // Q.a there is the single-particle kernel value: a mis-bound attribute or
        // label target, or a second material leaking in, would move it.
        const std::size_t a_centre = at(pixel_x(4.0), pixel_y(4.0));
        const std::size_t b_centre = at(pixel_x(4.0), pixel_y(4.5));
        PPR_TEST_ASSERT(frame.m_label[a_centre] == 11u);
        PPR_TEST_ASSERT(frame.m_label[b_centre] == 12u);
        PPR_TEST_ASSERT(frame.m_density[a_centre] > static_cast<float>(kDensityThreshold));
        PPR_TEST_ASSERT(frame.m_density[b_centre] > static_cast<float>(kDensityThreshold));

        // The overlapping band picks one owner per texel and accumulates only that
        // owner: the density there stays below the sum of both kernels.
        const std::size_t band = at(pixel_x(4.0), pixel_y(4.25));
        PPR_TEST_ASSERT(frame.m_label[band] == 11u or frame.m_label[band] == 12u);
        PPR_TEST_ASSERT(frame.m_density[band] > 0.0f);
        const DensityKernel neutral = densityKernel({1.0, 0.0}, 0.0, 0.0);
        const double one_particle_at_band = densityWeightCells({0.0, 0.25}, radius_m, neutral);
        PPR_TEST_ASSERT(static_cast<double>(frame.m_density[band]) <= 1.1 * one_particle_at_band + 1.0e-3);

        // The two materials' own stores differ, so the composite colours at their
        // centres differ: a single shared/albedo-fallback binding would not.
        const std::size_t composite_a = a_centre * 4u;
        const std::size_t composite_b = b_centre * 4u;
        PPR_TEST_ASSERT(frame.m_composite[composite_a + 3u] > 0.9f);
        PPR_TEST_ASSERT(frame.m_composite[composite_b + 3u] > 0.9f);
        PPR_TEST_ASSERT(frame.m_composite[composite_a] != frame.m_composite[composite_b]);
        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL particle_surface_two_materials labels a={} b={} band={} D_a={:.6f} "
                "D_b={:.6f} D_band={:.6f}",
                frame.m_label[a_centre], frame.m_label[b_centre], frame.m_label[band], frame.m_density[a_centre],
                frame.m_density[b_centre], frame.m_density[band]));
    };

    PPR_UNIT_TEST (particle_surface_two_classes_touch_at_one_owner_per_sample) {
        // One opaque (material 11) and one optical (material 13, authored alpha
        // 0x80/0xff = 0.501961) liquid touching. Both composites read the same
        // label, so exactly one class writes each sample: the opaque sample is fully
        // opaque, the optical sample carries its authored alpha, and neither
        // double-blends the other.
        constexpr double radius_m = 0.28209479177387814;
        DensityCohortSpec cohort{};
        cohort.m_optical_materials = {13u};
        cohort.m_particles.push_back(
            DensityParticleSpec{.m_x_m = 4.0, .m_y_m = 4.0, .m_radius_m = radius_m, .m_material = 11u});
        cohort.m_particles.push_back(
            DensityParticleSpec{.m_x_m = 4.0, .m_y_m = 4.5, .m_radius_m = radius_m, .m_material = 13u});
        const auto frames = runDensityCohort(cohort, 1u);
        PPR_TEST_ASSERT(frames.size() == 1u);
        const auto &frame = frames[0];
        constexpr u32 raster = 256u;
        const double pixel_world = 8.0 / 256.0;
        const auto pixel_x = [&](const double world_x) { return static_cast<u32>(std::floor(world_x / pixel_world)); };
        const auto pixel_y = [&](const double world_y) {
            return static_cast<u32>(std::floor((8.0 - world_y) / pixel_world));
        };
        const auto at = [&](const u32 x, const u32 y) { return std::size_t{y} * raster + x; };

        PPR_TEST_ASSERT(frame.m_phase_mask == 1u);
        PPR_TEST_ASSERT(
            frame.m_class_mask == ((1u << static_cast<u32>(EGridParticleDrawClass::opaque)) |
                                      (1u << static_cast<u32>(EGridParticleDrawClass::optical))));

        const std::size_t opaque_centre = at(pixel_x(4.0), pixel_y(4.0));
        const std::size_t optical_centre = at(pixel_x(4.0), pixel_y(4.5));
        PPR_TEST_ASSERT(frame.m_label[opaque_centre] == 11u);
        PPR_TEST_ASSERT(frame.m_label[optical_centre] == 13u);

        // Opaque liquid: the stored alpha is exactly one and the composite writes
        // C = 1 at the centre.
        PPR_TEST_ASSERT(std::abs(frame.m_normal_alpha[opaque_centre][3u] - frame.m_density[opaque_centre]) <= 1.0e-3f);
        PPR_TEST_ASSERT(frame.m_composite[opaque_centre * 4u + 3u] >= 0.99f);
        // Optical liquid: stored alpha is the authored 0.501961 weighted by the same
        // kernel, and the composite applies it once (0.501961 at full coverage).
        const float stored_alpha = frame.m_normal_alpha[optical_centre][3u];
        const float density = frame.m_density[optical_centre];
        PPR_TEST_ASSERT(density > static_cast<float>(kDensityThreshold));
        PPR_TEST_ASSERT(std::abs(stored_alpha / density - 0.501961f) <= 2.0e-3f);
        PPR_TEST_ASSERT(std::abs(frame.m_composite[optical_centre * 4u + 3u] - 0.501961f) <= 2.0e-2f);
        // A double blend would leave 1.0 here; a missed class rejection would too.
        PPR_TEST_ASSERT(frame.m_composite[optical_centre * 4u + 3u] < 0.95f);
        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL particle_surface_two_classes label_o={} label_x={} alpha_o={:.6f} "
                "alpha_x={:.6f} stored_x={:.6f} D_x={:.6f}",
                frame.m_label[opaque_centre], frame.m_label[optical_centre], frame.m_composite[opaque_centre * 4u + 3u],
                frame.m_composite[optical_centre * 4u + 3u], stored_alpha, density));
    };

    PPR_UNIT_TEST (particle_density_held_frame_repeats_the_field_and_the_lane_draws_directly_nowhere) {
        // A held publication keeps its own packed rows and stable IDs; the density
        // field is a view-dependent render product, so the same rows and camera must
        // reproduce the same field exactly (no cue advance, no accumulation history).
        DensityCohortSpec cohort{};
        constexpr double radius_m = 0.28209479177387814;
        for (u32 index = 0u; index < 5u; ++index) {
            cohort.m_particles.push_back(
                DensityParticleSpec{
                    .m_x_m = 3.5 + 0.5 * double(index % 3u),
                    .m_y_m = 3.5 + 0.5 * double(index / 3u),
                    .m_radius_m = radius_m});
        }

        const auto held = runDensityCohort(cohort, 2u, true);
        PPR_TEST_ASSERT(held.size() == 2u);
        PPR_TEST_ASSERT(held[0].m_density.size() == held[1].m_density.size());
        for (std::size_t index = 0u; index < held[0].m_density.size(); ++index) {
            PPR_TEST_ASSERT(held[0].m_density[index] == held[1].m_density[index]);
        }
        for (std::size_t index = 0u; index < held[0].m_composite.size(); ++index) {
            PPR_TEST_ASSERT(held[0].m_composite[index] == held[1].m_composite[index]);
        }
        // The labels are a per-frame render product too: a held frame re-derives
        // the same winner and the same attributes, not a retained label image.
        PPR_TEST_ASSERT(held[0].m_label.size() == held[1].m_label.size());
        for (std::size_t index = 0u; index < held[0].m_label.size(); ++index) {
            PPR_TEST_ASSERT(held[0].m_label[index] == held[1].m_label[index]);
        }

        // The routed rows must also not be shaded directly: the direct classes draw
        // the same prepared frame with no field, and the cleared label proves the
        // exclusion (an admitted material drawn directly would still leave the
        // field empty, so the label is asserted against the admitted value below).
        PPR_TEST_ASSERT(held[0].m_phase_mask == 1u);

        // The admitted lane's own rows must not be encoded by the direct classes.
        // The same prepared frame with only the direct classes must leave the target
        // at its cleared value everywhere.
        DensityCohortSpec direct = cohort;
        direct.m_direct_only = true;
        const auto direct_frames = runDensityCohort(direct, 1u);
        PPR_TEST_ASSERT(direct_frames.size() == 1u);
        for (const float value: direct_frames[0].m_composite)
            PPR_TEST_ASSERT(value == 0.0f);
        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL particle_density_hold rows=5 density_pixels={} direct_pixels={}",
                held[0].m_density.size(), direct_frames[0].m_composite.size()));
    };

    // ------------------------------------------------------------------
    // world-space surface smoothing GPU probes
    // ------------------------------------------------------------------
    // Span 64 at the design's raster scale: 540 raster pixels across 64 cells is
    // 8.4375 samples per cell, so sigma is 1.265625 samples and the loaded halo is
    // ceil(3 sigma) = 4 (the design's n = 4 / K = 9 row). Every probe below places
    // its geometry at this scale unless it says otherwise.
    //
    // The unit is RASTER PIXELS PER WORLD CELL (`samples_per_cell`), never cells
    // across the window: a span of S cells at this 540-pixel reference is 540/S.
    // Passing the span itself as the scale builds 1/S pixels per cell - a window a
    // fraction of a cell wide, i.e. the closest possible zoom rather than the
    // farthest.
    inline constexpr double kSpan64SamplesPerCell = 540.0 / 64.0;
    inline constexpr double kSpan220SamplesPerCell = 540.0 / 220.0;
    // The far reference: 540 px across 2048 cells is 0.263672 px/cell, so the shared
    // support is 0.1135 px and the four quadrature points of the texel at the
    // particle centre sit 0.948 cell away (rho = 2.203). The field is therefore
    // extinguished outright; the sampled extinction boundary is 0.580771 px/cell,
    // span 929.8, so this reference is 2.2x past it in rho.
    inline constexpr double kSpan2048SamplesPerCell = 540.0 / 2048.0;
    inline constexpr u32 kSpan64Halo = 4u;

    PPR_UNIT_TEST (particle_surface_filter_matches_the_documented_operator) {
        // The two smoothing entries must implement the documented operator, not an
        // approximation of it: sigma = .15 world cell, halo = ceil(3 sigma * samples
        // per cell), a tap only at the mask label, air keeping its weight and a
        // foreign nonempty label leaving the denominator. The cohort exercises both
        // branches at once - two liquids of one admitted class touching at .9 cell,
        // and an isolated droplet whose surface has to extend into air - and the CPU
        // restatement of the operator runs over the SAME read-back accumulation field
        // the dispatches consumed, so nothing but the filter arithmetic differs.
        constexpr double radius_m = 0.28209479177387814;
        constexpr u32 material_a = 11u;
        constexpr u32 material_b = 12u;
        constexpr u32 tint_a = 0xc080'40ffu;
        constexpr u32 tint_b = 0x40c0'80ffu;

        DensityCohortSpec cohort{};
        cohort.m_raster = 256u;
        cohort.m_world_extent_m = static_cast<float>(256.0 / kSpan64SamplesPerCell);
        cohort.m_materials = {
            DensityMaterialSpec{.m_material = material_a, .m_color_rgba = tint_a},
            DensityMaterialSpec{.m_material = material_b, .m_color_rgba = tint_b},
        };
        cohort.m_particles = {
            DensityParticleSpec{.m_x_m = 10.0, .m_y_m = 15.0, .m_radius_m = radius_m, .m_material = material_a},
            DensityParticleSpec{.m_x_m = 10.9, .m_y_m = 15.0, .m_radius_m = radius_m, .m_material = material_b},
            DensityParticleSpec{.m_x_m = 22.0, .m_y_m = 15.0, .m_radius_m = radius_m, .m_material = material_a},
        };

        const auto frames = runDensityCohort(cohort, 1u);
        PPR_TEST_ASSERT(frames.size() == 1u);
        const auto &frame = frames[0];
        // The frame used the design's own kernel parameters for this zoom.
        PPR_TEST_ASSERT(std::abs(double(frame.m_samples_per_cell) - kSpan64SamplesPerCell) <= 1.0e-3);
        PPR_TEST_ASSERT(frame.m_halo == kSpan64Halo);

        std::vector<SurfaceTexel> accumulation(std::size_t{frame.m_raster} * frame.m_raster);
        for (std::size_t texel = 0u; texel < accumulation.size(); ++texel) {
            accumulation[texel].m_label = frame.m_label[texel];
            accumulation[texel].m_base = Double3{
                double(frame.m_base[texel][0u]), double(frame.m_base[texel][1u]), double(frame.m_base[texel][2u])};
            accumulation[texel].m_density = double(frame.m_base[texel][3u]);
        }

        const std::vector<SurfaceTexel> predicted = surfaceFilterField(
            accumulation, frame.m_raster, kSpan64SamplesPerCell, frame.m_halo);

        const Double3 ratio_a = diagnosticTintCpu(tint_a);
        const Double3 ratio_b = diagnosticTintCpu(tint_b);
        PPR_TEST_ASSERT(std::abs(ratio_a.x - ratio_b.x) > 0.1);

        u32 labelled = 0u;
        u32 extended = 0u;
        std::vector<std::string> offenders;
        for (std::size_t texel = 0u; texel < predicted.size(); ++texel) {
            const SurfaceTexel &expected = predicted[texel];
            // The mask is an integer rule, so the two implementations must agree
            // exactly, including the extended labels the halo rule writes into air.
            PPR_TEST_ASSERT(frame.m_filtered_label[texel] == expected.m_label);
            if (expected.m_label != 0u) {
                ++labelled;
                if (frame.m_label[texel] == 0u)
                    ++extended;
            }

            const double actual_density = double(frame.m_filtered_base[texel][3u]);
            if (std::abs(actual_density - expected.m_density) > 5.0e-3 * expected.m_density + 2.0e-4) {
                if (offenders.size() < 4u) {
                    offenders.push_back(
                        std::format(
                            " ({},{})L={} D={:.6f} cpu={:.6f}", texel % frame.m_raster, texel / frame.m_raster,
                            frame.m_filtered_label[texel], actual_density, expected.m_density));
                }
            }

            PPR_TEST_ASSERT(std::abs(actual_density - expected.m_density) <= 5.0e-3 * expected.m_density + 2.0e-4);
            const std::array<double, 3u> expected_base{expected.m_base.x, expected.m_base.y, expected.m_base.z};
            for (u32 channel = 0u; channel < 3u; ++channel) {
                PPR_TEST_ASSERT(
                    std::abs(double(frame.m_filtered_base[texel][channel]) - expected_base[channel]) <=
                    5.0e-3 * std::abs(expected_base[channel]) + 2.0e-4);
            }

            // No bleed: a filtered texel's attribute ratio is ITS OWN label's
            // authored tint, so no foreign tap reached the numerator. Both tints are
            // distinct, so a blended numerator cannot pass. The ratio is independent
            // of the tap weights because every tap of one label carries one albedo.
            if (actual_density > 1.0e-2 and
                (frame.m_filtered_label[texel] == material_a or frame.m_filtered_label[texel] == material_b)) {
                const bool first = frame.m_filtered_label[texel] == material_a;
                const std::array<double, 3u> expected_ratio{
                    first ? ratio_a.x : ratio_b.x, first ? ratio_a.y : ratio_b.y, first ? ratio_a.z : ratio_b.z};
                for (u32 channel = 0u; channel < 3u; ++channel) {
                    PPR_TEST_ASSERT(
                        std::abs(
                            double(frame.m_filtered_base[texel][channel]) / actual_density - expected_ratio[channel]) <=
                        6.0e-3);
                }
            }
        }

        // Both branches of the mask actually ran: a labelled interior exists and the
        // filter extended labels into texels the accumulation left empty.
        PPR_TEST_ASSERT(labelled > 100u);
        PPR_TEST_ASSERT(extended > 0u);
        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL particle_surface_filter_operator labelled={} extended={} halo={} "
                "samples_per_cell={:.5f} offenders={}",
                labelled, extended, frame.m_halo, double(frame.m_samples_per_cell),
                describeAttributeOffenders(offenders)));
    };

    PPR_UNIT_TEST (particle_surface_filter_bridges_the_reference_gap_and_keeps_the_detached_pair) {
        // The revision's headline requirement. Two neutral same-label particles at
        // s = .90 cell have an initial contour gap of .141467 cell and must become one
        // contour after the .15-cell filter; the s = 1.00 reference pair (gap
        // .241467 cell) must stay detached. The design's discrete-shift evaluation
        // puts the span64 axis contact limit at .9834/.9876 cell, so both cases sit on
        // their intended side. Unfiltered, neither pair has any density at the saddle
        // because 2R = .860924 cell is shorter than both separations - filtering is
        // the only thing that can join them.
        constexpr double radius_m = 0.28209479177387814;
        // The saddle is placed exactly on a texel centre so the comparison is a
        // reproducible raster phase instead of a favourable subpixel position.
        constexpr double midpoint_x = (128.0 + 0.5) / kSpan64SamplesPerCell;

        struct ReferencePair {
            double m_raw{}; // accumulation density at the saddle texel
            double m_filtered{}; // smoothed density at the saddle texel
            double m_alpha{}; // resolved coverage of the saddle pixel
            // Minimum resolved coverage along the pair axis from centre to centre:
            // exactly the pixels a joined neck has to fill.
            double m_axis_min{1.0};
            u32 m_label{}; // smoothed label at the saddle
        };

        const auto run_pair = [&](const double spacing) {
            DensityCohortSpec cohort{};
            cohort.m_raster = 256u;
            cohort.m_world_extent_m = static_cast<float>(256.0 / kSpan64SamplesPerCell);
            cohort.m_particles = {
                DensityParticleSpec{.m_x_m = midpoint_x - 0.5 * spacing, .m_y_m = 15.0, .m_radius_m = radius_m},
                DensityParticleSpec{.m_x_m = midpoint_x + 0.5 * spacing, .m_y_m = 15.0, .m_radius_m = radius_m},
            };
            const auto frames = runDensityCohort(cohort, 1u);
            PPR_TEST_ASSERT(frames.size() == 1u);
            const auto &frame = frames[0];
            const u32 x = 128u;
            const auto y = static_cast<u32>(
                std::floor((double(cohort.m_world_extent_m) - 15.0) / (1.0 / kSpan64SamplesPerCell)));
            const std::size_t saddle = std::size_t{y} * frame.m_raster + x;
            ReferencePair pair{};
            pair.m_raw = double(frame.m_density[saddle]);
            pair.m_filtered = double(frame.m_filtered_density[saddle]);
            pair.m_alpha = double(frame.m_composite[saddle * 4u + 3u]);
            pair.m_label = frame.m_filtered_label[saddle];
            const auto first = static_cast<u32>(std::floor((midpoint_x - 0.5 * spacing) * kSpan64SamplesPerCell));
            const auto last = static_cast<u32>(std::floor((midpoint_x + 0.5 * spacing) * kSpan64SamplesPerCell));
            for (u32 axis_x = first; axis_x <= last and axis_x < frame.m_raster; ++axis_x) {
                pair.m_axis_min = (std::min) (pair.m_axis_min,
                    double(frame.m_composite[(std::size_t{y} * frame.m_raster + axis_x) * 4u + 3u]));
            }

            return pair;
        };

        const ReferencePair joined = run_pair(0.90);
        const ReferencePair detached = run_pair(1.00);
        // Log the state of both saddles BEFORE asserting: the interesting failure is a
        // pair that sits within one contour band of theta, and the numbers that
        // separate "bridged", "straddled" and "detached" are the field densities, the
        // resolved coverages and the axis minimum.
        const double saddle_x = midpoint_x;
        const double saddle_y = 15.0;
        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL particle_surface_filter_bridge saddle=({:.5f},{:.5f}) theta={:.6f} "
                "joined=raw:{:.6f} filtered:{:.6f}({:.4f} theta) alpha:{:.4f} axis_min:{:.4f} label:{} "
                "detached=raw:{:.6f} filtered:{:.6f}({:.4f} theta) alpha:{:.4f} axis_min:{:.4f} label:{}",
                saddle_x, saddle_y, kDensityThreshold, joined.m_raw, joined.m_filtered,
                joined.m_filtered / kDensityThreshold, joined.m_alpha, joined.m_axis_min, joined.m_label,
                detached.m_raw, detached.m_filtered, detached.m_filtered / kDensityThreshold, detached.m_alpha,
                detached.m_axis_min, detached.m_label));

        // Neither pair's contour reaches the saddle before the filter: the compact
        // support (2R = .860924 cell) is shorter than both separations, so the only
        // density there is quadrature leakage far below theta.
        PPR_TEST_ASSERT(joined.m_raw < 0.01 and detached.m_raw < 0.01);
        // The .141-cell gap bridges: the saddle rises through theta. The discrete-shift
        // reference puts the span64 axis contact limit at .98342 cell (design 7-E:
        // .9834-.9876), so s = .90 is 1.515 theta at the saddle texel.
        PPR_TEST_ASSERT(joined.m_filtered > kDensityThreshold);
        // The intended detached pair stays detached in the FIELD: 1.00 cell is 1.7 %
        // past the reference contact limit, so the saddle texel reads .954 theta (the
        // exact pair axis reads .908 theta) and no contour reaches the midpoint.
        PPR_TEST_ASSERT(detached.m_filtered < kDensityThreshold);

        // Rendered criterion, derived from the occupancy operator rather than from an
        // arbitrary alpha threshold: a sample is occupied iff D > theta, so a resolved
        // pixel's alpha is the fraction of its four samples that clear theta and a
        // joined neck must cover all four at every pixel along the pair axis. At
        // span64 one pixel is .1185 cell and the saddle density changes by ~.075 per
        // pixel, so a saddle within one sample spacing of theta may occupy only some
        // of a pixel's samples: the invariant that separates a bridge from a pinch is
        // therefore connection at full coverage, which is what a joined neck provides.
        PPR_TEST_ASSERT(joined.m_axis_min > 0.99 and joined.m_alpha > 0.99);
        PPR_TEST_ASSERT(detached.m_axis_min < 0.99 and detached.m_alpha < 0.99);
        // The two references must also stay distinguishable, not merely on one side of
        // a threshold: the detached saddle is materially less covered than the bridge.
        PPR_TEST_ASSERT(detached.m_alpha < joined.m_alpha);
        // Both saddles carry the pair's own label, so the coverage above is the pair's
        // and not another material's.
        PPR_TEST_ASSERT(joined.m_label != 0u and detached.m_label == joined.m_label);
    };

    PPR_UNIT_TEST (particle_surface_filter_keeps_the_isolated_droplet_growth_bounded) {
        // Design 7-E: the specified discrete stencil grows the neutral isolated
        // contour radius by at most 9.12 % at 1080/span64, so the accepted bound is
        // 9.5 % there. The same table already refutes a strict bound at span220 (the
        // droplet is under two raster pixels across), so that case is recorded.
        constexpr double radius_m = 0.28209479177387814;

        // The D = theta crossing along one axis, linearly interpolated between the two
        // texels that bracket it: a growth of .3 texels is resolvable this way even
        // though the raster itself stores one value per texel.
        const auto crossing = [](const std::vector<float> &density, const u32 raster, const u32 cx, const u32 cy,
                                  const int dx, const int dy) {
            const auto value = [&](const int x, const int y) {
                return double(density[std::size_t(y) * raster + std::size_t(x)]);
            };
            double previous = value(int(cx), int(cy));
            for (u32 step = 1u; step < 64u; ++step) {
                const int x = int(cx) + dx * int(step);
                const int y = int(cy) + dy * int(step);
                if (x < 0 or y < 0 or x >= int(raster) or y >= int(raster))
                    break;
                const double current = value(x, y);
                if (current < kDensityThreshold) {
                    const double gap = previous - kDensityThreshold;
                    const double drop = previous - current;
                    return double(step) - 1.0 + (drop > 0.0 ? gap / drop : 0.5);
                }

                previous = current;
            }

            return 0.0;
        };

        const auto growth = [&](const double samples_per_cell) {
            DensityCohortSpec cohort{};
            cohort.m_raster = 256u;
            cohort.m_world_extent_m = static_cast<float>(256.0 / samples_per_cell);
            // Texel-centre aligned (reference x and image y both land on texel 128):
            // the same raster phase for both fields.
            const double centre_x = 128.5 / samples_per_cell;
            const double centre_y = 127.5 / samples_per_cell;
            cohort.m_particles.push_back(
                DensityParticleSpec{.m_x_m = centre_x, .m_y_m = centre_y, .m_radius_m = radius_m});
            const auto frames = runDensityCohort(cohort, 1u);
            PPR_TEST_ASSERT(frames.size() == 1u);
            const auto &frame = frames[0];
            double before = 0.0;
            double after = 0.0;
            for (const std::array<int, 2u> &axis: {std::array<int, 2u>{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
                before += crossing(frame.m_density, frame.m_raster, 128u, 128u, axis[0u], axis[1u]);
                after += crossing(frame.m_filtered_density, frame.m_raster, 128u, 128u, axis[0u], axis[1u]);
            }

            PPR_TEST_ASSERT(before > 0.0);
            // The droplet must survive the filter at all.
            PPR_TEST_ASSERT(after / before > 0.5);
            return std::array<double, 3u>{before * 0.25, after * 0.25, after / before - 1.0};
        };

        const auto span64 = growth(kSpan64SamplesPerCell);
        const auto span220 = growth(kSpan220SamplesPerCell);
        // The design's resolved-reference bound, applied only at span64.
        PPR_TEST_ASSERT(span64[2u] <= 0.095);
        // Span220 keeps the droplet alive; its growth is recorded, not asserted
        // tightly: at 2.45 samples per cell the discrete stencil is anisotropic and
        // the design's own table already exceeds a universal 10 % claim.
        PPR_TEST_ASSERT(span220[2u] <= 0.05);
        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL particle_surface_filter_droplet span64=({:.4f}->{:.4f},{:+.2f}%) "
                "span220=({:.4f}->{:.4f},{:+.2f}%)",
                span64[0u], span64[1u], 100.0 * span64[2u], span220[0u], span220[1u], 100.0 * span220[2u]));
    };

    PPR_UNIT_TEST (particle_surface_filter_keeps_a_sheet_and_meets_the_reference_scallop) {
        // (a) A one-particle-thick sheet - the thinnest body this field can express -
        // must not erase under +/-3 sigma. Four same-label particles in a row at the
        // rest spacing keep one continuous contour along the spine, including the
        // midpoints where a single unfiltered neighbour carries the least weight.
        constexpr double radius_m = 0.28209479177387814;
        constexpr u32 sheet_particles = 4u;
        constexpr double sheet_spacing = 0.5;
        DensityCohortSpec sheet{};
        sheet.m_raster = 256u;
        sheet.m_world_extent_m = static_cast<float>(256.0 / kSpan64SamplesPerCell);
        const double sheet_start = 12.0;
        for (u32 index = 0u; index < sheet_particles; ++index) {
            sheet.m_particles.push_back(
                DensityParticleSpec{
                    .m_x_m = sheet_start + sheet_spacing * double(index), .m_y_m = 15.0, .m_radius_m = radius_m});
        }

        const auto sheet_frames = runDensityCohort(sheet, 1u);
        PPR_TEST_ASSERT(sheet_frames.size() == 1u);
        const auto &sheet_frame = sheet_frames[0];
        const auto sheet_y = static_cast<u32>(
            std::floor((double(sheet.m_world_extent_m) - 15.0) / (1.0 / kSpan64SamplesPerCell)));
        double spine_minimum = 1.0e30;
        double spine_maximum = -1.0e30;
        for (u32 step = 0u; step <= 32u; ++step) {
            const double world_x = sheet_start + sheet_spacing * double(sheet_particles - 1u) * double(step) / 32.0;
            const auto x = static_cast<u32>(std::floor(world_x * kSpan64SamplesPerCell));
            const double density = double(
                sheet_frame.m_filtered_density[std::size_t{sheet_y} * sheet_frame.m_raster + x]);
            spine_minimum = (std::min) (spine_minimum, density);
            spine_maximum = (std::max) (spine_maximum, density);
        }

        // Every spine sample is inside the drawn contour: the sheet is preserved, not
        // merely present at its particle centres.
        PPR_TEST_ASSERT(spine_minimum > kDensityThreshold);
        PPR_TEST_ASSERT(spine_maximum > kDensityThreshold);
        // And it survives into the rendered image along its whole length.
        for (u32 step = 0u; step <= 8u; ++step) {
            const double world_x = sheet_start + sheet_spacing * double(sheet_particles - 1u) * double(step) / 8.0;
            const auto x = static_cast<u32>(std::floor(world_x * kSpan64SamplesPerCell));
            PPR_TEST_ASSERT(
                sheet_frame.m_composite[(std::size_t{sheet_y} * sheet_frame.m_raster + x) * 4u + 3u] > 0.5f);
        }

        // (b) The amplitude targets the revision is pinned to, evaluated as the
        // design's own discrete-shift reference: 256 contour samples per period of
        // the neutral half-plane lattice, at the span64 scale. The unfiltered pair is
        // the model's own control (design section 1: .072189/.094361 cell at
        // s = .50/.55).
        const double rest_before = latticeContourAmplitude(0.50, kSpan64SamplesPerCell, 0u, radius_m);
        const double dilated_before = latticeContourAmplitude(0.55, kSpan64SamplesPerCell, 0u, radius_m);
        const double rest_after = latticeContourAmplitude(0.50, kSpan64SamplesPerCell, kSpan64Halo, radius_m);
        const double dilated_after = latticeContourAmplitude(0.55, kSpan64SamplesPerCell, kSpan64Halo, radius_m);

        PPR_TEST_ASSERT(std::abs(rest_before - 0.072189) <= 0.002);
        PPR_TEST_ASSERT(std::abs(dilated_before - 0.094361) <= 0.002);
        // The adopted targets: <= .006 cell at rest and <= .016 cell at 10 % dilation.
        PPR_TEST_ASSERT(rest_after <= 0.006);
        PPR_TEST_ASSERT(dilated_after <= 0.016);
        PPR_TEST_ASSERT(rest_after < rest_before and dilated_after < dilated_before);
        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL particle_surface_filter_scallop sheet_min={:.6f} sheet_max={:.6f} "
                "rest={:.6f}->{:.6f} dilated={:.6f}->{:.6f}",
                spine_minimum, spine_maximum, rest_before, rest_after, dilated_before, dilated_after));
    };

    PPR_UNIT_TEST (particle_surface_filter_refilters_on_a_camera_change_and_holds_bit_identical) {
        // The smoothed field is a view-dependent render product with no retained
        // history: a held publication at an unchanged camera must reproduce it bit for
        // bit, and a camera change must produce a genuinely re-filtered field.
        constexpr double radius_m = 0.28209479177387814;
        DensityCohortSpec cohort{};
        cohort.m_raster = 256u;
        cohort.m_world_extent_m = static_cast<float>(256.0 / kSpan64SamplesPerCell);
        for (u32 index = 0u; index < 5u; ++index) {
            cohort.m_particles.push_back(
                DensityParticleSpec{
                    .m_x_m = 12.0 + 0.5 * double(index % 3u),
                    .m_y_m = 14.0 + 0.5 * double(index / 3u),
                    .m_radius_m = radius_m});
        }

        const auto held = runDensityCohort(cohort, 2u, true);
        PPR_TEST_ASSERT(held.size() == 2u);
        PPR_TEST_ASSERT(held[0].m_filtered_density.size() == held[1].m_filtered_density.size());
        for (std::size_t texel = 0u; texel < held[0].m_filtered_density.size(); ++texel) {
            PPR_TEST_ASSERT(held[0].m_filtered_density[texel] == held[1].m_filtered_density[texel]);
            PPR_TEST_ASSERT(held[0].m_filtered_label[texel] == held[1].m_filtered_label[texel]);
            for (u32 channel = 0u; channel < 4u; ++channel) {
                PPR_TEST_ASSERT(held[0].m_filtered_base[texel][channel] == held[1].m_filtered_base[texel][channel]);
            }
        }

        // A quarter-pixel camera move changes both the accumulation and the smoothed
        // field; a retained image or a filter that only ran on new publications would
        // leave the second identical.
        DensityCohortSpec shifted = cohort;
        shifted.m_camera_offset_px = 0.25;
        const auto moved = runDensityCohort(shifted, 1u);
        PPR_TEST_ASSERT(moved.size() == 1u);
        u32 accumulation_changed = 0u;
        u32 filtered_changed = 0u;
        for (std::size_t texel = 0u; texel < moved[0].m_filtered_density.size(); ++texel) {
            if (moved[0].m_density[texel] != held[0].m_density[texel])
                ++accumulation_changed;
            if (moved[0].m_filtered_density[texel] != held[0].m_filtered_density[texel])
                ++filtered_changed;
        }

        PPR_TEST_ASSERT(accumulation_changed > 0u);
        PPR_TEST_ASSERT(filtered_changed > 0u);
        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL particle_surface_filter_hold held_identical=1 accumulation_changed={} "
                "filtered_changed={}",
                accumulation_changed, filtered_changed));
    };

    // ------------------------------------------------------------------
    // minimum-presence marker GPU probes
    // ------------------------------------------------------------------
    // The four standard 4x sample positions relative to a pixel's own centre, i.e.
    // the shader's fragmentGridReactive table shifted by -0.5. The composite
    // evaluates the marker at these positions, so a probe measuring the resolved
    // dot must too.
    inline constexpr std::array<std::array<double, 2u>, 4u> kMarkerSampleOffsets{{
        {-0.125, -0.375},
        {0.375, -0.125},
        {-0.375, 0.125},
        {0.125, 0.375},
    }};

    // F at one distance in RASTER SAMPLES from the marker's centre, restated from
    // grid_particle_density.slang particleMarkerCoverage: saturate((R - d)/(R - core))
    // with R = 1.0 and core = 0.5 raster samples, i.e. the design's 2/3/4 content-pixel
    // diameters converted by kMarkerContentPixelsPerRasterSample. Exactly 1 at the
    // opaque core radius, exactly .5 at the half-coverage radius, exactly 0 at the
    // coverage radius.
    [[nodiscard]] double markerCoverageAtDistance(const double distance_px) {
        const double span = kMarkerRadiusPx - kMarkerCoreRadiusPx;
        return std::clamp((kMarkerRadiusPx - distance_px) / span, 0.0, 1.0);
    }

    // The analytic resolved PEAK of the marker at a known subpixel phase: the largest
    // four-sample mean of F over the pixels around a particle that sits `offset_px`
    // raster samples in -x from its containing pixel's centre (the fixture's +x camera
    // shift moves the dot left). This is the closed form the subpixel probe measures,
    // not a fitted value: 1.0 at the centred phase, .916146 at a quarter sample and
    // .678459 at a half sample, with the profile's own worst phase (a pixel corner) at
    // .455325. It also states the mechanism: a whole pixel reads 1 only while all four
    // of its samples - a square of circumradius hypot(.125,.375) = .39528 raster
    // samples about the pixel centre - stay inside the .5-raster-sample opaque core.
    [[nodiscard]] double markerAnalyticPeak(const double offset_px) {
        const double particle_x = -offset_px;
        double peak = 0.0;
        for (int pixel_y = -2; pixel_y <= 2; ++pixel_y) {
            for (int pixel_x = -2; pixel_x <= 2; ++pixel_x) {
                double sum = 0.0;
                for (const auto &offset: kMarkerSampleOffsets) {
                    sum += markerCoverageAtDistance(
                        std::hypot(particle_x - (double(pixel_x) + offset[0u]), 0.0 - (double(pixel_y) + offset[1u])));
                }

                peak = (std::max) (peak, sum * 0.25);
            }
        }

        return peak;
    }

    // F at one scene sample of a pixel, restated from the read-back winner record:
    // the same profile, with d measured from the stored nearest centre to the
    // sample's own position inside that pixel, both in raster samples. This is the
    // shader's arithmetic, not a fitted approximation of it.
    [[nodiscard]] double markerCoverageAtSample(
        const DensityFrameResult &frame, const u32 x, const u32 y, const double offset_x, const double offset_y) {
        const std::array<double, 4u> &record = frame.m_marker[std::size_t{y} * frame.m_raster + x];
        if (record[2u] <= 0.0) {
            return 0.0;
        }

        const double dx = record[0u] - offset_x;
        const double dy = record[1u] - offset_y;
        return markerCoverageAtDistance(std::sqrt(dx * dx + dy * dy));
    }

    // Diagnostic record of the field at one span: every texel whose accumulation or
    // filtered label is nonzero, listed up to `limit` with its label, its raw Q.a in
    // both bundles and the marker record at the same texel. A span past the sampled
    // extinction must report none; any entry here names the texel that still
    // resolves the row instead of failing an anonymous count.
    [[nodiscard]] std::string describeLabelledTexels(const DensityFrameResult &frame, const u32 limit) {
        u32 total = 0u;
        u32 listed = 0u;
        std::string record;
        for (u32 y = 0u; y < frame.m_raster; ++y) {
            for (u32 x = 0u; x < frame.m_raster; ++x) {
                const std::size_t index = std::size_t{y} * frame.m_raster + x;
                if (frame.m_filtered_label[index] == 0u and frame.m_label[index] == 0u) {
                    continue;
                }

                ++total;
                if (listed >= limit) {
                    continue;
                }

                ++listed;
                std::array<double, 4u> record_here = frame.m_marker[index];
                record += std::format(
                    " [({},{}) label={} accumulated_label={} q_a={:.6f} filtered_q_a={:.6f} "
                    "marker=({:.3f},{:.3f},material={})]",
                    x, y, frame.m_filtered_label[index], frame.m_label[index], double(frame.m_density[index]),
                    double(frame.m_filtered_density[index]), record_here[0u], record_here[1u],
                    static_cast<u32>(record_here[2u]));
            }
        }

        return std::format("labelled_texels={} listed={}{}", total, listed, record);
    }

    PPR_UNIT_TEST (particle_surface_marker_keeps_an_isolated_particle_at_every_span) {
        // Revision 8-B: an unoccluded particle in the viewport interior keeps a
        // content-pixel dot - a 2-content-pixel opaque core, a 3-content-pixel
        // half-coverage diameter and a 4-content-pixel outer diameter - at every zoom,
        // because a separate unfiltered screen-space marker is unioned with the
        // physical body as C = max(B, F). Three references, all at the design's
        // 540-pixel raster and all named in samples per cell (540/S):
        //
        //   span64   8.4375 px/cell: the filtered body is about 7 raster samples
        //            across, so the 2-raster-sample marker sits inside it and must win
        //            NOWHERE, leaving the composite the revision-7 image;
        //   span220  2.4545 px/cell: the body is about 1.9 raster samples across and
        //            the droplet survives on its own label;
        //   span2048 0.263672 px/cell: the support is 0.1135 px and every quadrature
        //            point of the nearest texel is at rho = 2.203, so the field
        //            provably cannot represent the droplet at all and the marker is
        //            the only thing that can.
        constexpr double radius_m = 0.28209479177387814;
        constexpr u32 raster = 256u;

        // The design contract is in CONTENT pixels and the shader measures in RASTER
        // samples, which the presentation grid makes exactly two content pixels each
        // (resolved = ceil(window/P), raster = ceil(resolved/2)). The three radii are
        // one rule - the opaque core one content pixel inside the coverage radius, the
        // half-coverage radius half a content pixel inside it - and the conversion is
        // the only free parameter.
        PPR_TEST_ASSERT(kMarkerContentPixelsPerRasterSample == 2.0);
        PPR_TEST_ASSERT(kMarkerCoreRadiusContentPx == kMarkerOuterRadiusContentPx - 1.0);
        PPR_TEST_ASSERT(kMarkerHalfCoverageRadiusContentPx == kMarkerOuterRadiusContentPx - 0.5);
        PPR_TEST_ASSERT(2.0 * kMarkerCoreRadiusContentPx == 2.0);
        PPR_TEST_ASSERT(2.0 * kMarkerHalfCoverageRadiusContentPx == 3.0);
        PPR_TEST_ASSERT(2.0 * kMarkerOuterRadiusContentPx == 4.0);
        PPR_TEST_ASSERT(kMarkerCoreRadiusPx == 0.5 and kMarkerHalfCoverageRadiusPx == 0.75 and kMarkerRadiusPx == 1.0);
        PPR_TEST_ASSERT(kMarkerQuadRadiusPx == kMarkerRadiusPx + 0.7071067811865476);
        // The profile at those radii: exactly 1 at the 1-content-pixel opaque core,
        // exactly .5 at the 1.5-content-pixel half-coverage radius and exactly 0 at the
        // 2-content-pixel outer radius, with a one-content-pixel ramp.
        PPR_TEST_ASSERT(markerCoverageAtDistance(kMarkerCoreRadiusPx) == 1.0);
        PPR_TEST_ASSERT(std::abs(markerCoverageAtDistance(kMarkerHalfCoverageRadiusPx) - 0.5) <= 1.0e-15);
        PPR_TEST_ASSERT(markerCoverageAtDistance(kMarkerRadiusPx) == 0.0);
        PPR_TEST_ASSERT(markerCoverageAtDistance(2.0 * kMarkerRadiusPx) == 0.0);

        const auto run = [&](const double samples_per_cell) {
            DensityCohortSpec cohort{};
            cohort.m_raster = raster;
            cohort.m_world_extent_m = static_cast<float>(double(raster) / samples_per_cell);
            // One texel-centre phase: raster x = 128.5 and image row = 127.5 both put
            // the particle exactly on a pixel centre, so the marker's distance field
            // and the resolved image share one phase and the diameters below are not
            // measuring the phase instead of the dot.
            cohort.m_particles.push_back(
                DensityParticleSpec{
                    .m_x_m = 128.5 / samples_per_cell,
                    .m_y_m = (double(raster) - 127.5) / samples_per_cell,
                    .m_radius_m = radius_m});
            return runDensityCohort(cohort, 1u);
        };

        // The particle's own raster pixel, in image coordinates.
        constexpr u32 particle_x = 128u;
        constexpr u32 particle_y = 127u;

        // --- span 64: a resolved body swallows the marker ----------------
        {
            const auto frames = run(kSpan64SamplesPerCell);
            PPR_TEST_ASSERT(frames.size() == 1u);
            const auto &frame = frames[0];

            // The marker record exists at the particle's own texel (otherwise the
            // "the marker changed nothing" result below would be vacuous).
            const std::array<double, 4u> &record = frame.m_marker[std::size_t{particle_y} * raster + particle_x];
            PPR_TEST_ASSERT(record[2u] == 1.0);
            PPR_TEST_ASSERT(std::hypot(record[0u], record[1u]) <= 0.5 + 1.0e-3);
            PPR_TEST_ASSERT(markerCoverageAtSample(frame, particle_x, particle_y, 0.0, 0.0) >= 1.0 - 1.0e-3);

            // The binary body occupancy B at one scene sample of one pixel, restated
            // from the smoothed field the composite actually reconstructs.
            const auto texel = [&](const int ix, const int iy) {
                SurfaceTexel value{};
                if (ix < 0 or iy < 0 or ix >= int(raster) or iy >= int(raster)) {
                    return value;
                }

                const std::size_t index = std::size_t(iy) * frame.m_raster + std::size_t(ix);
                value.m_label = frame.m_filtered_label[index];
                value.m_base = Double3{
                    double(frame.m_filtered_base[index][0u]), double(frame.m_filtered_base[index][1u]),
                    double(frame.m_filtered_base[index][2u])};
                value.m_density = double(frame.m_filtered_base[index][3u]);
                value.m_alpha = double(frame.m_filtered_normal_alpha[index][3u]);
                return value;
            };
            const auto body_at = [&](const double sx, const double sy) -> double {
                const SurfaceTexel nearest = texel(int(std::floor(sx)), int(std::floor(sy)));
                if (nearest.m_label == 0u) {
                    return 0.0;
                }

                const int base_x = int(std::floor(sx - 0.5));
                const int base_y = int(std::floor(sy - 0.5));
                const std::array<SurfaceTexel, 4u> taps{
                    texel(base_x, base_y), texel(base_x + 1, base_y), texel(base_x, base_y + 1),
                    texel(base_x + 1, base_y + 1)};
                const auto reconstructed = densityReconstruct(
                    taps, nearest, sx - 0.5 - double(base_x), sy - 0.5 - double(base_y), false);
                if (not reconstructed.m_valid) {
                    return 0.0;
                }

                return densityOccupancy(reconstructed.m_density);
            };

            double worst_marker_excess = -1.0;
            double worst_alpha_gap = 0.0;
            for (u32 y = 0u; y < raster; ++y) {
                for (u32 x = 0u; x < raster; ++x) {
                    double resolved_body = 0.0;
                    for (const auto &offset: kMarkerSampleOffsets) {
                        const double sample_x = double(x) + 0.5 + offset[0u];
                        const double sample_y = double(y) + 0.5 + offset[1u];
                        const double body = body_at(sample_x, sample_y);
                        const double marker = markerCoverageAtSample(frame, x, y, offset[0u], offset[1u]);
                        // B wins every tie, so ANY sample where the marker exceeds the
                        // body would have changed the image.
                        worst_marker_excess = (std::max) (worst_marker_excess, marker - body);
                        resolved_body += body;
                    }

                    const double resolved = double(frame.m_composite[(std::size_t{y} * raster + x) * 4u + 3u]);
                    worst_alpha_gap = (std::max) (worst_alpha_gap, std::abs(resolved - resolved_body * 0.25));
                }
            }

            // The marker never wins inside this body, and the resolved image is the
            // four-sample body occupancy: max(B, F) == B here, so the union is
            // bit-identical to the body alone.
            PPR_TEST_ASSERT(worst_marker_excess <= 1.0e-9);
            PPR_TEST_ASSERT(worst_alpha_gap <= 2.0e-3);
            _.log(
                std::format(
                    "WORLD_LIGHTING_MATERIAL particle_surface_marker_body samples_per_cell={:.5f} "
                    "marker_excess={:.9f} alpha_gap={:.6f} peak={:.4f}",
                    kSpan64SamplesPerCell, worst_marker_excess, worst_alpha_gap,
                    double(frame.m_composite[(std::size_t{particle_y} * raster + particle_x) * 4u + 3u])));
        }

        // --- span 64 / 220 / 2048: the same dot at every zoom -------------
        const auto measure = [&](const double samples_per_cell) {
            const auto frames = run(samples_per_cell);
            PPR_TEST_ASSERT(frames.size() == 1u);
            const auto &frame = frames[0];
            const std::size_t centre = std::size_t{particle_y} * raster + particle_x;

            // The nearest pixel's record and its coverage at the pixel centre: the
            // guarantee's own quantity (a distance of at most .5 raster samples is
            // inside the .5-raster-sample opaque core, so F == 1 there).
            const std::array<double, 4u> &record = frame.m_marker[centre];
            PPR_TEST_ASSERT(record[2u] == 1.0);
            const double offset = std::hypot(record[0u], record[1u]);
            PPR_TEST_ASSERT(offset <= 0.5 + 1.0e-3);
            const double peak_marker = markerCoverageAtSample(frame, particle_x, particle_y, 0.0, 0.0);
            PPR_TEST_ASSERT(peak_marker >= 1.0 - 1.0e-3);

            // The record field in the units the shader measures in. A raster texel
            // holds a record exactly when its own centre is inside the candidate
            // bound, its stored offset is that centre's distance to the particle (unorm
            // quantum 8/65535), and F at that centre is the analytic profile of the
            // SAME distance. A radius left in content pixels, or a missing /2, breaks
            // the equality at every texel past the first.
            const double centre_x = double(particle_x) + 0.5;
            const double centre_y = double(particle_y) + 0.5;
            double worst_record_gap = 0.0;
            double worst_offset_gap = 0.0;
            u32 records = 0u;
            for (u32 y = particle_y - 4u; y <= particle_y + 4u; ++y) {
                for (u32 x = particle_x - 4u; x <= particle_x + 4u; ++x) {
                    const std::array<double, 4u> &here = frame.m_marker[std::size_t{y} * raster + x];
                    const double distance = std::hypot(double(x) + 0.5 - centre_x, double(y) + 0.5 - centre_y);
                    if (here[2u] <= 0.0) {
                        PPR_TEST_ASSERT(distance > kMarkerQuadRadiusPx - 1.0e-3);
                        continue;
                    }

                    ++records;
                    PPR_TEST_ASSERT(distance <= kMarkerQuadRadiusPx + 1.0e-3);
                    const double stored = std::hypot(here[0u], here[1u]);
                    worst_offset_gap = (std::max) (worst_offset_gap, std::abs(stored - distance));
                    worst_record_gap = (std::max) (worst_record_gap,
                        std::abs(markerCoverageAtSample(frame, x, y, 0.0, 0.0) - markerCoverageAtDistance(distance)));
                }
            }

            // The candidate bound is a circle of 1.7071 raster samples, so exactly the
            // 3x3 block of texel centres around the particle holds a record.
            PPR_TEST_ASSERT(records == 9u);
            PPR_TEST_ASSERT(worst_offset_gap <= 2.0e-3);
            PPR_TEST_ASSERT(worst_record_gap <= 3.0e-3);

            u32 labelled = 0u;
            u32 covered = 0u;
            double sum_alpha = 0.0;
            for (u32 pixel = 0u; pixel < raster * raster; ++pixel) {
                if (frame.m_label[pixel] != 0u)
                    ++labelled;
                if (frame.m_composite[pixel * 4u + 3u] > 0.0f)
                    ++covered;
                sum_alpha += double(frame.m_composite[pixel * 4u + 3u]);
            }

            // The resolved dot along +x in raster samples. The centre pixel is fully
            // covered at THIS phase because all four of its samples sit at radius
            // hypot(.125,.375) = .39528 raster samples from its centre, inside the
            // .5-raster-sample opaque core, so each reaches F == 1. (A quarter-sample
            // phase moves that worst sample to .53028 and the pixel reads .916146;
            // the phase-independent floor is the subpixel probe's subject.) The +1
            // sample pixel is the partial outer fringe and +2 samples - four content
            // pixels - is already outside the outer radius.
            std::array<double, 3u> resolved_axis{};
            for (u32 step = 0u; step < 3u; ++step) {
                resolved_axis[step] = double(
                    frame.m_composite[(std::size_t{particle_y} * raster + particle_x + step) * 4u + 3u]);
            }

            _.log(
                std::format(
                    "WORLD_LIGHTING_MATERIAL particle_surface_marker_span samples_per_cell={:.6f} span_cells={:.3f} "
                    "centre_offset={:.5f} marker_peak={:.4f} resolved=({:.4f},{:.4f},{:.4f}) records={} "
                    "record_gap={:.6f} offset_gap={:.6f} covered={} labelled={} alpha_sum={:.4f} {}",
                    samples_per_cell, 540.0 / samples_per_cell, offset, peak_marker, resolved_axis[0u],
                    resolved_axis[1u], resolved_axis[2u], records, worst_record_gap, worst_offset_gap, covered,
                    labelled, sum_alpha, describeLabelledTexels(frame, 4u)));
            return std::array<double, 6u>{
                peak_marker, resolved_axis[0u], resolved_axis[1u], resolved_axis[2u], double(covered),
                double(labelled)};
        };

        const auto span64 = measure(kSpan64SamplesPerCell);
        const auto span220 = measure(kSpan220SamplesPerCell);
        const auto far = measure(kSpan2048SamplesPerCell);

        // The nearest resolved pixel is fully covered at every zoom. This is the
        // survival guarantee: at span2048 the density field is empty (asserted
        // below), so only the marker can produce it.
        for (const auto &measured: {span64, span220, far}) {
            PPR_TEST_ASSERT(measured[1u] >= 1.0 - 1.0e-3);
            PPR_TEST_ASSERT(measured[0u] >= 1.0 - 1.0e-3);
        }

        // At the far span the surface support is 0.1135 px and the nearest texel's
        // quadrature points are 0.948 cell away (rho = 2.203): the field must be
        // empty everywhere, including every label, so the dot is the marker alone.
        // The labelled set is logged with its labels, its raw Q.a and the marker
        // record at the same texel before it is asserted empty.
        {
            const auto frames = run(kSpan2048SamplesPerCell);
            PPR_TEST_ASSERT(frames.size() == 1u);
            const auto &frame = frames[0];
            u32 labelled = 0u;
            double peak_density = 0.0;
            for (std::size_t texel_index = 0u; texel_index < frame.m_filtered_density.size(); ++texel_index) {
                if (frame.m_filtered_label[texel_index] != 0u)
                    ++labelled;
                peak_density = (std::max) (peak_density, double(frame.m_filtered_density[texel_index]));
            }

            _.log(
                std::format(
                    "WORLD_LIGHTING_MATERIAL particle_surface_marker_far_field samples_per_cell={:.6f} "
                    "span_cells={:.3f} support_px={:.4f} peak_filtered_density={:.9f} {}",
                    kSpan2048SamplesPerCell, 540.0 / kSpan2048SamplesPerCell,
                    kDensitySurfaceRadiusScale * radius_m / kSpan2048SamplesPerCell, peak_density,
                    describeLabelledTexels(frame, 8u)));

            PPR_TEST_ASSERT(labelled == 0u);
            PPR_TEST_ASSERT(peak_density == 0.0);
            PPR_TEST_ASSERT(far[0u] >= 1.0 - 1.0e-3);
            // The marker is the ONLY contributor here, so a resolved pixel's alpha is
            // the mean of its four per-sample coverages of the one profile (the
            // material is opaque, so opacity multiplies by one).
            double worst_dot_gap = 0.0;
            for (u32 y = 0u; y < raster; ++y) {
                for (u32 x = 0u; x < raster; ++x) {
                    double resolved = 0.0;
                    for (const auto &offset: kMarkerSampleOffsets) {
                        resolved += markerCoverageAtSample(frame, x, y, offset[0u], offset[1u]);
                    }

                    const double alpha = double(frame.m_composite[(std::size_t{y} * raster + x) * 4u + 3u]);
                    worst_dot_gap = (std::max) (worst_dot_gap, std::abs(alpha - resolved * 0.25));
                }
            }

            PPR_TEST_ASSERT(worst_dot_gap <= 2.0e-3);
            // The resolved dot along +x: fully covered at the centre, a partial outer
            // fringe at +1 raster sample (two content pixels) and nothing at +2 raster
            // samples (four content pixels), never a larger blob.
            PPR_TEST_ASSERT(far[2u] > 0.05 and far[2u] < 0.95);
            PPR_TEST_ASSERT(far[3u] <= 1.0e-6);
            PPR_TEST_ASSERT(far[4u] >= 1.0 and far[4u] <= 25.0);
        }

        // Span220 keeps the droplet's own label: the marker only adds the fringe a
        // 1.9-sample body cannot cover, it does not replace the field.
        PPR_TEST_ASSERT(span220[5u] > 0.0);
        PPR_TEST_ASSERT(span64[5u] > 0.0);
    };

    PPR_UNIT_TEST (particle_surface_marker_is_a_union_and_never_a_second_draw) {
        // "No double draw" in the union's own terms: two coincident liquid particles
        // must produce ONE winner record and ONE coverage, because the marker is a
        // winner selection and a max, never an additive layer.
        //
        // The cohort sits past the sampled extinction (0.263672 px/cell, rho = 2.203
        // at the nearest quadrature point) on purpose: the density field SUMS every
        // coincident row's weight, so at any resolving zoom the second particle
        // legitimately doubles Q.a and the two cohorts differ through the body, not
        // through the marker. Isolating the marker requires a field that contributes
        // nothing at all - which is exactly the far-zoom case this increment exists
        // for.
        constexpr double radius_m = 0.28209479177387814;
        constexpr u32 raster = 256u;

        const auto run = [&](const u32 count) {
            DensityCohortSpec cohort{};
            cohort.m_raster = raster;
            cohort.m_world_extent_m = static_cast<float>(double(raster) / kSpan2048SamplesPerCell);
            for (u32 index = 0u; index < count; ++index) {
                cohort.m_particles.push_back(
                    DensityParticleSpec{
                        .m_x_m = 128.5 / kSpan2048SamplesPerCell,
                        .m_y_m = (double(raster) - 127.5) / kSpan2048SamplesPerCell,
                        .m_radius_m = radius_m});
            }

            return runDensityCohort(cohort, 1u);
        };

        const auto single = run(1u);
        const auto doubled = run(2u);
        PPR_TEST_ASSERT(single.size() == 1u and doubled.size() == 1u);
        PPR_TEST_ASSERT(single[0].m_marker.size() == doubled[0].m_marker.size());
        for (std::size_t texel_index = 0u; texel_index < single[0].m_marker.size(); ++texel_index) {
            for (u32 channel = 0u; channel < 4u; ++channel) {
                PPR_TEST_ASSERT(single[0].m_marker[texel_index][channel] == doubled[0].m_marker[texel_index][channel]);
            }
        }

        // The resolved image is identical too: the second coincident candidate is
        // occluded by the first at the same distance, so coverage never doubles.
        PPR_TEST_ASSERT(single[0].m_composite.size() == doubled[0].m_composite.size());
        u32 different = 0u;
        for (std::size_t value = 0u; value < single[0].m_composite.size(); ++value) {
            if (single[0].m_composite[value] != doubled[0].m_composite[value])
                ++different;
        }

        PPR_TEST_ASSERT(different == 0u);
        const std::size_t centre = std::size_t{127u} * raster + 128u;
        PPR_TEST_ASSERT(double(doubled[0].m_composite[centre * 4u + 3u]) <= 1.0 + 1.0e-6);
        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL particle_surface_marker_union single_alpha={:.6f} double_alpha={:.6f} "
                "different={}",
                double(single[0].m_composite[centre * 4u + 3u]), double(doubled[0].m_composite[centre * 4u + 3u]),
                different));
    };

    PPR_UNIT_TEST (particle_surface_marker_is_stable_under_subpixel_translation) {
        // A subpixel camera move must translate the dot, not make it appear and
        // disappear: the resolved coverage is the four-sample AA of the same
        // 1-raster-sample (2-content-pixel) disc, so its integrated area is stable
        // while its centroid follows the camera. The marker is recomputed from the
        // camera like the field, and a held publication at an unchanged camera
        // reproduces it bit for bit. The cohort is the same past-extinction far
        // reference as the union probe, so every measured alpha belongs to the marker
        // alone.
        constexpr double radius_m = 0.28209479177387814;
        constexpr u32 raster = 256u;

        const auto run = [&](const double camera_offset_px, const u32 frames) {
            DensityCohortSpec cohort{};
            cohort.m_raster = raster;
            cohort.m_world_extent_m = static_cast<float>(double(raster) / kSpan2048SamplesPerCell);
            cohort.m_camera_offset_px = camera_offset_px;
            cohort.m_particles.push_back(
                DensityParticleSpec{
                    .m_x_m = 128.5 / kSpan2048SamplesPerCell,
                    .m_y_m = (double(raster) - 127.5) / kSpan2048SamplesPerCell,
                    .m_radius_m = radius_m});
            return runDensityCohort(cohort, frames, frames > 1u);
        };

        // The dot's own measurements: integrated alpha, peak alpha, the count of
        // half-covered pixels and the alpha-weighted centroid.
        const auto measure = [&](const DensityFrameResult &frame) {
            double area = 0.0;
            double peak = 0.0;
            double weight = 0.0;
            double centroid_x = 0.0;
            double centroid_y = 0.0;
            u32 half_covered = 0u;
            for (u32 y = 0u; y < raster; ++y) {
                for (u32 x = 0u; x < raster; ++x) {
                    const double alpha = double(frame.m_composite[(std::size_t{y} * raster + x) * 4u + 3u]);
                    area += alpha;
                    peak = (std::max) (peak, alpha);
                    if (alpha >= 0.5)
                        ++half_covered;
                    weight += alpha;
                    centroid_x += alpha * (double(x) + 0.5);
                    centroid_y += alpha * (double(y) + 0.5);
                }
            }

            PPR_TEST_ASSERT(weight > 0.0);
            return std::array<double, 5u>{area, peak, double(half_covered), centroid_x / weight, centroid_y / weight};
        };

        const auto base = run(0.0, 1u);
        PPR_TEST_ASSERT(base.size() == 1u);
        const auto base_measure = measure(base[0]);
        // A 1-raster-sample (2-content-pixel) coverage radius with the 2:1 ramp
        // profile integrates to 4 pi / 6 = 2 pi / 3 = 2.094 over the plane, but the
        // 4-per-pixel sample lattice resolves it to 1.8213 / 1.8408 / 1.8603 at the
        // three probed phases (a ~13% lattice undershoot, computed from the same
        // profile), and the half-covered pixel count is 1 / 1 / 2. The windows below
        // are those discrete values: they exclude both a vanished dot and a
        // revision-8-sized one (~8.4 area, 3+ half-covered pixels).
        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL particle_surface_marker_base area={:.6f} peak={:.6f} "
                "half_covered={} centroid_x={:.4f} analytic_peak={:.6f}",
                base_measure[0u], base_measure[1u], base_measure[2u], base_measure[3u], markerAnalyticPeak(0.0)));
        // At the exactly centred phase all four samples of the pixel sit at
        // .39528 raster samples from the particle, inside the .5-raster-sample opaque
        // core, so every sample reads exactly 1 and the pixel resolves to 1. This is
        // the one phase where alpha == 1 is a property of the contract.
        PPR_TEST_ASSERT(base_measure[1u] >= 1.0 - 1.0e-3);
        PPR_TEST_ASSERT(base_measure[0u] >= 1.5 and base_measure[0u] <= 2.2);
        PPR_TEST_ASSERT(base_measure[2u] >= 1.0 and base_measure[2u] <= 3.0);

        std::array<double, 3u> areas{};
        for (u32 step = 0u; step < 3u; ++step) {
            const double offset_px = 0.25 * double(step);
            const auto moved = step == 0u ? base : run(offset_px, 1u);
            PPR_TEST_ASSERT(moved.size() == 1u);
            const auto measured = measure(moved[0]);
            const double analytic_peak = markerAnalyticPeak(offset_px);
            // Logged before asserting: a drifting peak reports both sides.
            _.log(
                std::format(
                    "WORLD_LIGHTING_MATERIAL particle_surface_marker_phase offset_px={:.3f} peak={:.6f} "
                    "analytic_peak={:.6f} area={:.6f} half_covered={}",
                    offset_px, measured[1u], analytic_peak, measured[0u], measured[2u]));
            // A peak of 1 requires all FOUR samples of one pixel inside the
            // 1-content-pixel (.5-raster-sample) opaque core. Those four samples form a
            // square of circumradius hypot(.125,.375) = .39528 raster samples about the
            // pixel centre, so the farthest one is already .39528 away and a
            // displacement grows it by up to the full offset: a whole pixel saturates
            // only while the particle stays within .10472 raster samples of a pixel
            // centre (.13501 in the most favourable direction). It is therefore NOT a
            // phase-independent property, and the previous "peak == 1 at every phase"
            // assertion only held for the revision-8 dot, whose 1-raster-sample core
            // still covered the farthest sample at the three phases this probe visits
            // (.53028 at the half-sample phase). The phase-independent guarantees, both
            // derived from the profile alone, are:
            //   * the nearest sample is never farther than the 4x lattice's covering
            //     radius, .39276 raster samples at a pixel corner, which is inside the
            //     .5 raster sample core; that sample is fully covered, so the
            //     containing pixel keeps alpha >= .25 and the dot never disappears;
            //   * the profile's worst phase is the pixel corner, where the resolved
            //     peak is exactly .455325, so no phase falls below .45.
            // The exact resolved peak at each probed phase is pinned instead of a
            // constant, so a radius, record or sample-offset regression still fails.
            PPR_TEST_ASSERT(measured[1u] >= 0.45);
            PPR_TEST_ASSERT(std::abs(measured[1u] - analytic_peak) <= 2.0e-3);
            // The half-covered count is 1 / 1 / 2 at the three probed phases (only a
            // pixel holding all four samples inside the core reaches .5 at the centred
            // and quarter phases; the half-sample phase splits one full pixel across
            // the two it straddles), so 3 is already a revision-8-sized blob.
            PPR_TEST_ASSERT(measured[2u] >= 1.0 and measured[2u] <= 3.0);
            areas[step] = measured[0u];

            // The dot follows the camera: its alpha-weighted centroid moves by the
            // same subpixel amount, in the fixture's own sign convention for a +x
            // camera shift.
            if (step != 0u) {
                const double moved_x = measured[3u] - base_measure[3u];
                PPR_TEST_ASSERT(std::abs(std::abs(moved_x) - offset_px) <= 0.1);
                PPR_TEST_ASSERT(std::abs(measured[4u] - base_measure[4u]) <= 0.1);
            }
        }

        // No flicker: the integrated coverage of the same disc is stable across the
        // phases instead of alternating between two footprints. A dot 2 raster samples
        // across is sampled by 2x2 pixels of the 4-per-pixel lattice, so its lattice
        // sum varies more with phase than the wider revision-8 dot did; the window
        // below is a no-flicker bound, not an exact area.
        const double smallest = (std::min) ({areas[0u], areas[1u], areas[2u]});
        const double largest = (std::max) ({areas[0u], areas[1u], areas[2u]});
        PPR_TEST_ASSERT(largest <= smallest * 1.25);

        // Held publication, unchanged camera: the marker and the composite are
        // reproduced bit for bit; a camera move changes the record, so nothing is a
        // retained presence image.
        const auto held = run(0.0, 2u);
        PPR_TEST_ASSERT(held.size() == 2u);
        for (std::size_t texel_index = 0u; texel_index < held[0].m_marker.size(); ++texel_index) {
            for (u32 channel = 0u; channel < 4u; ++channel) {
                PPR_TEST_ASSERT(held[0].m_marker[texel_index][channel] == held[1].m_marker[texel_index][channel]);
            }
        }

        const auto shifted_frames = run(0.25, 1u);
        PPR_TEST_ASSERT(shifted_frames.size() == 1u);
        u32 moved_texels = 0u;
        for (std::size_t texel_index = 0u; texel_index < held[0].m_marker.size(); ++texel_index) {
            if (held[0].m_marker[texel_index] != shifted_frames[0].m_marker[texel_index])
                ++moved_texels;
        }

        PPR_TEST_ASSERT(moved_texels > 0u);
        _.log(
            std::format(
                "WORLD_LIGHTING_MATERIAL particle_surface_marker_subpixel areas=({:.4f},{:.4f},{:.4f}) "
                "peak={:.4f} half_covered={} centroid_x={:.4f} moved_texels={}",
                areas[0u], areas[1u], areas[2u], base_measure[1u], base_measure[2u], base_measure[3u], moved_texels));
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
                detail::WorldLightingMaterial::presentation_tint_rgb9e5_matches_the_authored_palette_bytes,
                detail::WorldLightingMaterial::presentation_motion_half_round_trips_and_clamps,
                detail::WorldLightingMaterial::presentation_material_word_reconstructs_the_canonical_pack_cell_word,
                detail::WorldLightingMaterial::presentation_position_precision_budget_pins_the_far_corner_quantum,
                detail::WorldLightingMaterial::presentation_row_layout_matches_the_cpu_twin,
                detail::WorldLightingMaterial::particle_descriptions_use_the_rows_own_phase_material,
                detail::WorldLightingMaterial::particle_shapes_share_one_support_bound,
                detail::WorldLightingMaterial::particle_solid_rim_is_analytic_coverage,
                detail::WorldLightingMaterial::particle_flow_cue_stretches_the_major_axis_only,
                detail::WorldLightingMaterial::particle_flow_cue_is_the_identity_at_zero_motion,
                detail::WorldLightingMaterial::particle_flow_cue_samples_a_per_cell_motion_field,
                detail::WorldLightingMaterial::particle_flow_cue_renormalises_partial_taps,
                detail::WorldLightingMaterial::particle_tangent_contour_stays_inside_the_support,
                detail::WorldLightingMaterial::particle_tangent_follows_the_fill_field,
                detail::WorldLightingMaterial::particle_tangent_energy_gate_fades_weak_fields,
                detail::WorldLightingMaterial::particle_tangent_gate_sequence_reduces_weak_flips_only,
                detail::WorldLightingMaterial::particle_plateau_coverage_is_conditional,
                detail::WorldLightingMaterial::particle_surface_constants_match_revision_five,
                detail::WorldLightingMaterial::particle_surface_isolated_neutral_draws_the_apparent_radius,
                detail::WorldLightingMaterial::particle_surface_integrated_weight_is_the_reference_area,
                detail::WorldLightingMaterial::particle_surface_kernel_is_zero_at_and_beyond_the_support,
                detail::WorldLightingMaterial::particle_surface_lattice_closes_the_rest_grid,
                detail::WorldLightingMaterial::particle_surface_label_selection_keeps_the_first_stable_row,
                detail::WorldLightingMaterial::particle_surface_categorical_reconstruction_never_filters_ids,
                detail::WorldLightingMaterial::particle_surface_two_liquids_of_one_class_keep_one_owner_per_texel,
                detail::WorldLightingMaterial::
                    particle_surface_deferred_composite_premultiplies_once_and_guards_the_field,
                detail::WorldLightingMaterial::particle_surface_reconstruction_keeps_the_true_edge_coordinate,
                detail::WorldLightingMaterial::particle_surface_categorical_derivative_matches_a_finite_difference,
                detail::WorldLightingMaterial::particle_surface_terrain_exclusion_masks_each_quadrature_point,
                // The one-texel contour threshold moved into the deferred-composite
                // probe when the density field was replaced by L/Z/Q/G.
                detail::WorldLightingMaterial::particle_density_empty_lane_clears_and_one_splat_integrates_to_its_area,
                detail::WorldLightingMaterial::
                    particle_density_lattice_saturates_and_holds_area_under_subpixel_translation,
                detail::WorldLightingMaterial::particle_density_span_and_droplet_cohorts_keep_the_reference_area,
                detail::WorldLightingMaterial::particle_surface_material_border_matches_the_quadrature_oracle,
                detail::WorldLightingMaterial::particle_surface_equal_distance_competition_keeps_the_first_row,
                detail::WorldLightingMaterial::particle_surface_authored_emission_is_added_once,
                detail::WorldLightingMaterial::particle_surface_composite_uses_four_distinct_scene_samples,
                detail::WorldLightingMaterial::particle_surface_two_materials_of_one_class_hold_one_label_per_texel,
                detail::WorldLightingMaterial::particle_surface_two_classes_touch_at_one_owner_per_sample,
                detail::WorldLightingMaterial::
                    particle_density_held_frame_repeats_the_field_and_the_lane_draws_directly_nowhere,
                // Revision 7 world-space smoothing: the operator, the reference gap
                // bridge, the droplet bound, sheet survival with the amplitude
                // targets, and the held/re-filtered lifetime.
                detail::WorldLightingMaterial::particle_surface_filter_matches_the_documented_operator,
                detail::WorldLightingMaterial::
                    particle_surface_filter_bridges_the_reference_gap_and_keeps_the_detached_pair,
                detail::WorldLightingMaterial::particle_surface_filter_keeps_the_isolated_droplet_growth_bounded,
                detail::WorldLightingMaterial::particle_surface_filter_keeps_a_sheet_and_meets_the_reference_scallop,
                detail::WorldLightingMaterial::
                    particle_surface_filter_refilters_on_a_camera_change_and_holds_bit_identical,
                // Revision 8 minimum-presence marker: the survival guarantee at every
                // span, the union that never double-draws, and its subpixel stability.
                detail::WorldLightingMaterial::particle_surface_marker_keeps_an_isolated_particle_at_every_span,
                detail::WorldLightingMaterial::particle_surface_marker_is_a_union_and_never_a_second_draw,
                detail::WorldLightingMaterial::particle_surface_marker_is_stable_under_subpixel_translation,
                detail::WorldLightingMaterial::particle_classes_declare_their_state_and_draw_once_per_class,
                detail::WorldLightingMaterial::held_publication_reculls_retained_rows_against_the_current_camera,
                detail::WorldLightingMaterial::held_recull_keeps_global_ascending_ids_across_blocks,
                detail::WorldLightingMaterial::
                    held_publication_keeps_the_accepted_liquid_lane_after_a_rejected_inventory,
                detail::WorldLightingMaterial::opaque_terrain_ownership_occludes_solid_and_liquid_particles,
                detail::WorldLightingMaterial::excavated_vacuum_survives_and_opaque_terrain_clips_the_particle,
            });
        };
        return group;
    }
}
