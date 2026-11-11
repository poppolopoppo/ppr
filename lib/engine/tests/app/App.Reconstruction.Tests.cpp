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
    // reconstruction: independent CPU lattice and temporal oracle
    // ------------------------------------------------------------------

    namespace Reconstruction {
        [[nodiscard]] constexpr bool same(const int2 left, const int2 right) noexcept {
            return left.x == right.x and left.y == right.y;
        }

        [[nodiscard]] constexpr bool same(const float2 left, const float2 right) noexcept {
            return left.x == right.x and left.y == right.y;
        }

        void assertClose(const float3 actual, const float3 expected, const float tolerance) {
            PPR_TEST_ASSERT(std::abs(actual.x - expected.x) <= tolerance);
            PPR_TEST_ASSERT(std::abs(actual.y - expected.y) <= tolerance);
            PPR_TEST_ASSERT(std::abs(actual.z - expected.z) <= tolerance);
        }

        namespace Oracle {
            // Standard 4x pattern in raster top-left coordinates, sample-index
            // order. Node (1,0) is sample 0 at (3/8,1/8), and so on.
            const std::array<int2, 4> kSampleNodes{int2{1, 0}, int2{3, 1}, int2{0, 2}, int2{2, 3}};
            const std::array<float2, 4> kSamplePositions{
                float2{3.0f / 8.0f, 1.0f / 8.0f}, float2{7.0f / 8.0f, 3.0f / 8.0f}, float2{1.0f / 8.0f, 5.0f / 8.0f},
                float2{5.0f / 8.0f, 7.0f / 8.0f}};

            struct Sample {
                int2 m_pixel{};
                u32 m_index{};

                [[nodiscard]] bool operator==(const Sample &) const noexcept = default;
            };

            struct Value {
                float3 m_color{};
                float3 m_minimum{};
                float3 m_maximum{};
            };

            [[nodiscard]] i32 floorDiv(const i32 value, const i32 divisor) noexcept {
                i32 quotient = value / divisor;
                const i32 remainder = value % divisor;
                if (remainder != 0 and ((remainder < 0) != (divisor < 0))) {
                    --quotient;
                }

                return quotient;
            }

            [[nodiscard]] i32 floorMod(const i32 value, const i32 divisor) noexcept {
                i32 remainder = value % divisor;
                if (remainder != 0 and ((remainder < 0) != (divisor < 0))) {
                    remainder += divisor;
                }

                return remainder;
            }

            // Geometric known-node test: a node is known when its absolute
            // quarter-pixel position equals one of the four sample positions
            // inside its containing source pixel. This does not use the
            // shader's residue tables or left/up formulas.
            [[nodiscard]] u32 knownIndex(const int2 node) noexcept {
                const int2 pixel{floorDiv(node.x, 4), floorDiv(node.y, 4)};
                const float2 absolute{
                    (static_cast<float>(node.x) + 0.5f) * 0.25f, (static_cast<float>(node.y) + 0.5f) * 0.25f};
                for (u32 index = 0u; index < kSamplePositions.size(); ++index) {
                    const float2 expected = float2{
                        static_cast<float>(pixel.x) + kSamplePositions[index].x,
                        static_cast<float>(pixel.y) + kSamplePositions[index].y};
                    if (std::abs(absolute.x - expected.x) <= 1.0e-6f and std::abs(absolute.y - expected.y) <= 1.0e-6f) {
                        return index;
                    }
                }

                return 4u;
            }

            [[nodiscard]] float2 stationaryPoint(const int2 pixel, const u32 sample, const float2 jitter) noexcept {
                return 2.0f * (float2{static_cast<float>(pixel.x), static_cast<float>(pixel.y)} +
                                  kSamplePositions[sample] -
                                  jitter);
            }

            [[nodiscard]] bool sourceSampleValid(
                const int2 pixel, const u32 sample, const int2 raster_extent, const int2 resolved_extent,
                const float2 jitter) noexcept {
                const float2 stationary = stationaryPoint(pixel, sample, jitter);
                return pixel.x >= 0 and
                       pixel.y >= 0 and
                       pixel.x < raster_extent.x and
                       pixel.y < raster_extent.y and
                       stationary.x >= 0.0f and
                       stationary.y >= 0.0f and
                       stationary.x < static_cast<float>(resolved_extent.x) and
                       stationary.y < static_cast<float>(resolved_extent.y);
            }

            [[nodiscard]] std::optional<Sample> nearestValidSample(
                const int2 requested_pixel, const u32 requested_sample, const int2 raster_extent,
                const int2 resolved_extent, const float2 jitter) noexcept {
                const float2 requested = stationaryPoint(requested_pixel, requested_sample, jitter);
                const int2 clamped{
                    std::clamp(requested_pixel.x, 0, raster_extent.x - 1),
                    std::clamp(requested_pixel.y, 0, raster_extent.y - 1)};
                const int2 low{std::max(0, clamped.x - 1), std::max(0, clamped.y - 1)};
                const int2 high{
                    std::min(raster_extent.x - 1, clamped.x + 1), std::min(raster_extent.y - 1, clamped.y + 1)};

                float best_distance = std::numeric_limits<float>::max();
                i32 best_order = std::numeric_limits<i32>::max();
                std::optional<Sample> best{};
                for (i32 y = low.y; y <= high.y; ++y) {
                    for (i32 x = low.x; x <= high.x; ++x) {
                        for (u32 sample = 0u; sample < kSamplePositions.size(); ++sample) {
                            if (not sourceSampleValid(int2{x, y}, sample, raster_extent, resolved_extent, jitter)) {
                                continue;
                            }

                            const float2 delta = stationaryPoint(int2{x, y}, sample, jitter) - requested;
                            const float distance = delta.x * delta.x + delta.y * delta.y;
                            const i32 order = y * raster_extent.x + x;
                            if (distance < best_distance or (distance == best_distance and order < best_order)) {
                                best_distance = distance;
                                best_order = order;
                                best = Sample{int2{x, y}, sample};
                            }
                        }
                    }
                }

                return best;
            }

            [[nodiscard]] std::optional<Sample> resolveValidSample(
                const int2 pixel, const u32 sample, const int2 raster_extent, const int2 resolved_extent,
                const float2 jitter) noexcept {
                if (sourceSampleValid(pixel, sample, raster_extent, resolved_extent, jitter)) {
                    return Sample{pixel, sample};
                }

                return nearestValidSample(pixel, sample, raster_extent, resolved_extent, jitter);
            }

            [[nodiscard]] std::optional<Sample> resolveKnownNode(
                const int2 node, const int2 raster_extent, const int2 resolved_extent, const float2 jitter) noexcept {
                const u32 known = knownIndex(node);
                if (known == 4u) {
                    return std::nullopt;
                }

                return resolveValidSample(
                    int2{floorDiv(node.x, 4), floorDiv(node.y, 4)}, known, raster_extent, resolved_extent, jitter);
            }

            struct KnownNode {
                int2 m_node{};
                Sample m_sample{};
            };

            // Brute-force scan in one lattice direction until a known node is
            // found. Known nodes repeat at most every four nodes, so eight steps
            // is a strict upper bound for both positive and negative nodes.
            [[nodiscard]] std::optional<KnownNode> findNearestKnown(
                const int2 node, const int2 direction, const int2 raster_extent, const int2 resolved_extent,
                const float2 jitter) noexcept {
                for (i32 step = 1; step <= 8; ++step) {
                    const int2 candidate{node.x + direction.x * step, node.y + direction.y * step};
                    if (knownIndex(candidate) == 4u) {
                        continue;
                    }

                    const auto sample = resolveKnownNode(candidate, raster_extent, resolved_extent, jitter);
                    if (not sample) {
                        return std::nullopt;
                    }
                    return KnownNode{candidate, *sample};
                }

                return std::nullopt;
            }

            [[nodiscard]] std::size_t sampleIndex(
                const int2 raster_extent, const int2 pixel, const u32 sample) noexcept {
                return (static_cast<std::size_t>(pixel.y) * static_cast<std::size_t>(raster_extent.x) +
                           static_cast<std::size_t>(pixel.x)) *
                           kSamplePositions.size() +
                       sample;
            }

            [[nodiscard]] float3 loadSample(
                const std::span<const float3> samples, const int2 raster_extent, const Sample &sample) noexcept {
                return samples[sampleIndex(raster_extent, sample.m_pixel, sample.m_index)];
            }

            [[nodiscard]] float luminance(const float3 color) noexcept {
                return 0.25f * color.x + 0.5f * color.y + 0.25f * color.z;
            }

            [[nodiscard]] std::optional<Value> reconstructNode(
                const int2 node, const std::span<const float3> samples, const int2 raster_extent,
                const int2 resolved_extent, const float2 jitter) noexcept {
                const u32 known = knownIndex(node);
                if (known != 4u) {
                    const auto sample = resolveValidSample(
                        int2{floorDiv(node.x, 4), floorDiv(node.y, 4)}, known, raster_extent, resolved_extent, jitter);
                    if (not sample) {
                        return std::nullopt;
                    }

                    const float3 color = loadSample(samples, raster_extent, *sample);
                    return Value{color, color, color};
                }

                const auto left = findNearestKnown(node, int2{-1, 0}, raster_extent, resolved_extent, jitter);
                const auto right = findNearestKnown(node, int2{1, 0}, raster_extent, resolved_extent, jitter);
                const auto up = findNearestKnown(node, int2{0, -1}, raster_extent, resolved_extent, jitter);
                const auto down = findNearestKnown(node, int2{0, 1}, raster_extent, resolved_extent, jitter);
                if (not left or not right or not up or not down) {
                    return std::nullopt;
                }

                const float3 left_color = loadSample(samples, raster_extent, left->m_sample);
                const float3 right_color = loadSample(samples, raster_extent, right->m_sample);
                const float3 up_color = loadSample(samples, raster_extent, up->m_sample);
                const float3 down_color = loadSample(samples, raster_extent, down->m_sample);

                const float horizontal = std::max(1.0e-5f, std::abs(luminance(left_color) - luminance(right_color)));
                const float vertical = std::max(1.0e-5f, std::abs(luminance(up_color) - luminance(down_color)));
                const float weight = vertical / (horizontal + vertical);
                const float3 color = weight * 0.5f * (left_color + right_color) +
                                     (1.0f - weight) * 0.5f * (up_color + down_color);
                const float3 minimum = pP::min(pP::min(left_color, right_color), pP::min(up_color, down_color));
                const float3 maximum = pP::max(pP::max(left_color, right_color), pP::max(up_color, down_color));
                return Value{color, minimum, maximum};
            }

            [[nodiscard]] std::optional<Value> reconstructResolved(
                const int2 pixel, const std::span<const float3> samples, const int2 raster_extent,
                const int2 resolved_extent, const float2 jitter) noexcept {
                const int2 shift{static_cast<i32>(4.0f * jitter.x), static_cast<i32>(4.0f * jitter.y)};
                const int2 base{2 * pixel.x + shift.x, 2 * pixel.y + shift.y};

                float3 color{0.0f, 0.0f, 0.0f};
                float3 minimum{
                    std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                    std::numeric_limits<float>::max()};
                float3 maximum{
                    -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(),
                    -std::numeric_limits<float>::max()};
                for (i32 dy = 0; dy < 2; ++dy) {
                    for (i32 dx = 0; dx < 2; ++dx) {
                        const auto node = reconstructNode(
                            int2{base.x + dx, base.y + dy}, samples, raster_extent, resolved_extent, jitter);
                        if (not node) {
                            return std::nullopt;
                        }

                        color += node->m_color;
                        minimum = pP::min(minimum, node->m_minimum);
                        maximum = pP::max(maximum, node->m_maximum);
                    }
                }

                color *= 0.25f;
                return Value{color, minimum, maximum};
            }

            [[nodiscard]] float3 probeColor(const int2 pixel, const u32 sample, const float tint) noexcept {
                const u32 r = static_cast<u32>(pixel.x * 13 + pixel.y * 7 + static_cast<i32>(sample) * 3) & 15u;
                const u32 g = static_cast<u32>(pixel.x * 5 + pixel.y * 11 + static_cast<i32>(sample) * 7 + 3) & 15u;
                const u32 b = static_cast<u32>(pixel.x * 3 + pixel.y * 2 + static_cast<i32>(sample) * 13 + 7) & 15u;
                return float3{
                    static_cast<float>(r) / 16.0f * tint, static_cast<float>(g) / 16.0f * tint,
                    static_cast<float>(b) / 16.0f * tint};
            }

            [[nodiscard]] std::vector<float3> makeProbeSamples(const int2 raster_extent, const float tint = 1.0f) {
                std::vector<float3> samples(
                    static_cast<std::size_t>(raster_extent.x) *
                    static_cast<std::size_t>(raster_extent.y) *
                    kSamplePositions.size());
                for (i32 y = 0; y < raster_extent.y; ++y) {
                    for (i32 x = 0; x < raster_extent.x; ++x) {
                        for (u32 sample = 0u; sample < kSamplePositions.size(); ++sample) {
                            samples[sampleIndex(raster_extent, int2{x, y}, sample)] = probeColor(
                                int2{x, y}, sample, tint);
                        }
                    }
                }

                return samples;
            }

            [[nodiscard]] float2 visibleNdc(const float2 resolved, const int2 window, const u32 pixel_scale) {
                return float2{
                    2.0f * static_cast<float>(pixel_scale) * resolved.x / static_cast<float>(window.x) - 1.0f,
                    1.0f - 2.0f * static_cast<float>(pixel_scale) * resolved.y / static_cast<float>(window.y)};
            }

            [[nodiscard]] float2 resolvedFromNdc(const float2 ndc, const int2 window, const u32 pixel_scale) {
                const float scale = static_cast<float>(pixel_scale);
                return float2{
                    (ndc.x + 1.0f) * 0.5f * static_cast<float>(window.x) / scale,
                    (1.0f - ndc.y) * 0.5f * static_cast<float>(window.y) / scale};
            }

            struct Reprojection {
                float2 m_pixel{};
                bool m_valid{};
            };

            [[nodiscard]] Reprojection reproject(
                const CameraSnapshot &current, const CameraSnapshot &previous, const PresentationGrid &grid,
                const PresentationGrid &previous_grid, const float2 current_pixel) {
                const float2 ndc = visibleNdc(current_pixel, grid.m_window, grid.m_pixel_scale);
                const float4 near_clip{ndc.x, ndc.y, 0.0f, 1.0f};
                const float4 far_clip{ndc.x, ndc.y, 1.0f, 1.0f};
                float4 near_world = near_clip * current.m_invert_view_projection;
                float4 far_world = far_clip * current.m_invert_view_projection;
                if (std::abs(near_world[3]) < 1.0e-12f or std::abs(far_world[3]) < 1.0e-12f) {
                    return {};
                }

                near_world = near_world / near_world[3];
                far_world = far_world / far_world[3];
                const float depth_delta = far_world[2] - near_world[2];
                if (std::abs(depth_delta) < 1.0e-12f) {
                    return {};
                }

                const float t = -near_world[2] / depth_delta;
                const float3 world{
                    near_world[0] + t * (far_world[0] - near_world[0]),
                    near_world[1] + t * (far_world[1] - near_world[1]),
                    near_world[2] + t * (far_world[2] - near_world[2])};
                const float4 previous_clip = float4{world.x, world.y, world.z, 1.0f} * previous.m_view_projection;
                if (previous_clip[3] <= 1.0e-12f) {
                    return {};
                }

                const float2 previous_ndc{previous_clip[0] / previous_clip[3], previous_clip[1] / previous_clip[3]};
                const float2 previous_pixel = resolvedFromNdc(
                    previous_ndc, previous_grid.m_window, previous_grid.m_pixel_scale);
                return Reprojection{
                    previous_pixel, std::isfinite(previous_pixel.x) and std::isfinite(previous_pixel.y)};
            }

            [[nodiscard]] std::optional<float3> sampleHistory(
                const std::span<const float3> history, const int2 backing_extent, const int2 resolved_extent,
                const float2 previous_pixel) noexcept {
                const float2 centre = previous_pixel - 0.5f;
                const int2 base{static_cast<i32>(std::floor(centre.x)), static_cast<i32>(std::floor(centre.y))};
                const float2 fraction = centre - float2{static_cast<float>(base.x), static_cast<float>(base.y)};
                const std::array<float, 2> weights_x{1.0f - fraction.x, fraction.x};
                const std::array<float, 2> weights_y{1.0f - fraction.y, fraction.y};

                float3 color{0.0f, 0.0f, 0.0f};
                for (i32 y = 0; y < 2; ++y) {
                    for (i32 x = 0; x < 2; ++x) {
                        const float weight = weights_x[static_cast<std::size_t>(x)] *
                                             weights_y[static_cast<std::size_t>(y)];
                        if (weight <= 0.0f) {
                            continue;
                        }

                        const int2 texel{base.x + x, base.y + y};
                        if (texel.x < 0 or
                            texel.y < 0 or
                            texel.x >= resolved_extent.x or
                            texel.y >= resolved_extent.y) {
                            return std::nullopt;
                        }

                        color += weight *
                                 history
                                     [static_cast<std::size_t>(texel.y) * static_cast<std::size_t>(backing_extent.x) +
                                         static_cast<std::size_t>(texel.x)];
                    }
                }

                return color;
            }

            [[nodiscard]] float currentWeight(const float motion_pixels) noexcept {
                const float motion_t = std::clamp((motion_pixels - 0.1f) / 0.4f, 0.0f, 1.0f);
                return 0.05f + (0.25f - 0.05f) * motion_t;
            }

            [[nodiscard]] std::vector<float3> expectedFrame(
                const std::span<const float3> samples, const PresentationGrid &grid, const CameraSnapshot &camera,
                const u32 phase, const bool history_valid, const std::span<const float3> previous_history,
                const PresentationGrid &previous_grid, const CameraSnapshot &previous_camera, const float tint = 1.0f) {
                std::vector<float3> expected(
                    static_cast<std::size_t>(grid.m_backing.x) * static_cast<std::size_t>(grid.m_backing.y));
                const float2 jitter{
                    0.5f * static_cast<float>(phase & 1u), 0.5f * static_cast<float>((phase >> 1u) & 1u)};

                for (i32 y = 0; y < grid.m_backing.y; ++y) {
                    for (i32 x = 0; x < grid.m_backing.x; ++x) {
                        const int2 pixel{x, y};
                        const auto current = reconstructResolved(
                            pixel, samples, grid.m_raster, grid.m_resolved, jitter);
                        PPR_TEST_ASSERT(current.has_value());
                        // The probe writes the hash pattern scaled by `tint`;
                        // reconstruction and its bounds are linear in the samples.
                        const float3 current_color = current->m_color * tint;
                        const float3 current_minimum = current->m_minimum * tint;
                        const float3 current_maximum = current->m_maximum * tint;
                        float3 color = current_color;
                        if (history_valid) {
                            const float2 current_pixel{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f};
                            const Reprojection reprojection = reproject(
                                camera, previous_camera, grid, previous_grid, current_pixel);
                            if (reprojection.m_valid) {
                                const auto history = sampleHistory(
                                    previous_history, previous_grid.m_backing, previous_grid.m_resolved,
                                    reprojection.m_pixel);
                                if (history) {
                                    const float motion = std::hypot(
                                        reprojection.m_pixel.x - current_pixel.x,
                                        reprojection.m_pixel.y - current_pixel.y);
                                    const float weight = currentWeight(motion);
                                    color = current_color * weight +
                                            pP::min(pP::max(*history, current_minimum), current_maximum) *
                                                (1.0f - weight);
                                }
                            }
                        }

                        expected
                            [static_cast<std::size_t>(y) * static_cast<std::size_t>(grid.m_backing.x) +
                                static_cast<std::size_t>(x)] = color;
                    }
                }

                return expected;
            }
        } // namespace Oracle

        // ------------------------------------------------------------------
        // independent camera fixtures
        // ------------------------------------------------------------------

        [[nodiscard]] CameraSnapshot makeOrthoCamera(const int2 window, const float3 origin, const float zoom = 1.0f) {
            CameraSnapshot camera{};
            camera.m_origin = origin;
            camera.m_view = float4x4::translate(-origin.x, -origin.y, -origin.z);
            camera.m_projection = rhi::getOrthoMatrix(
                static_cast<float>(window.x) / zoom, static_cast<float>(window.y) / zoom);
            camera.m_view_projection = camera.m_view * camera.m_projection;
            camera.m_invert_view = inverse(camera.m_view);
            camera.m_invert_projection = inverse(camera.m_projection);
            camera.m_invert_view_projection = inverse(camera.m_view_projection);
            camera.m_aspect_ratio = static_cast<float>(window.x) / static_cast<float>(window.y);
            camera.m_viewport_size = float2{static_cast<float>(window.x), static_cast<float>(window.y)};
            return camera;
        }

        // ------------------------------------------------------------------
        // GPU fixture: real scene target, pass, probe writer and fence
        // ------------------------------------------------------------------

        namespace Gpu {
            struct ProbeParameters {
                int2 m_extent{};
                u32 m_mode{};
                u32 m_reserved{};
                float4 m_base{};
            };
            static_assert(sizeof(ProbeParameters) == 32u);

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
                    if (mantissa == 0u) {
                        bits = sign << 31u;
                    } else {
                        const float subnormal = std::ldexp(static_cast<float>(mantissa), -24);
                        return sign != 0u ? -subnormal : subnormal;
                    }
                } else {
                    bits = (sign << 31u) | ((exponent - 15u + 127u) << 23u) | (mantissa << 13u);
                }

                return std::bit_cast<float>(bits);
            }

            struct Readback {
                std::vector<float3> m_rgb{};
                std::vector<float> m_alpha{};
            };


            struct Fixture {
                safe_ptr<IRhiService> m_rhi{IRhiService::get()};
                safe_ptr<IShaderService> m_shaders{IShaderService::get()};
                rhi::ComPtr<rhi::ICommandQueue> m_queue{};
                FrameFence m_fence{};
                ReconstructionPass m_pass{};
                rhi::ComPtr<rhi::IShaderProgram> m_probe_program{};
                rhi::ComPtr<rhi::IRenderPipeline> m_probe_pipeline{};
                rhi::ComPtr<rhi::IShaderObject> m_probe_root{};
                rhi::ComPtr<rhi::IShaderProgram> m_dump_program{};
                rhi::ComPtr<rhi::IComputePipeline> m_dump_pipeline{};
                rhi::ComPtr<rhi::IShaderObject> m_dump_root{};
                FenceCompletionValue m_last_value{kFenceValueNone};

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

                    shader::SharedModule module{};
                    PPR_TEST_ASSERT(not m_shaders->loadModuleFromFile(
                        std::filesystem::current_path() / "shaders" / "tests" / "reconstruction_probe.slang",
                        "reconstruction_probe", module.writeRef()));
                    const char *const entries[] = {"vertexReconstructionProbe", "fragmentReconstructionProbe"};
                    shader::ComPtr<slang::IEntryPoint> vertex;
                    shader::ComPtr<slang::IEntryPoint> fragment;
                    PPR_TEST_ASSERT(not make_error_code(module->findEntryPointByName(entries[0], vertex.writeRef())));
                    PPR_TEST_ASSERT(not make_error_code(module->findEntryPointByName(entries[1], fragment.writeRef())));
                    slang::IComponentType *components[] = {vertex.get(), fragment.get()};
                    rhi::ShaderProgramDesc description{};
                    description.linkingStyle = rhi::LinkingStyle::SingleProgram;
                    description.slangGlobalScope = module.get();
                    description.slangEntryPoints = components;
                    description.slangEntryPointCount = 2u;
                    shader::Diagnose diagnostics;
                    PPR_TEST_ASSERT(not make_error_code(
                        device.createShaderProgram(description, m_probe_program.writeRef(), diagnostics.writeRef())));

                    rhi::ColorTargetDesc target{};
                    target.format = rhi::Format::RGBA16Float;
                    rhi::RenderPipelineDesc pipeline{};
                    pipeline.program = m_probe_program;
                    pipeline.primitiveTopology = rhi::PrimitiveTopology::TriangleList;
                    pipeline.targets = &target;
                    pipeline.targetCount = 1u;
                    pipeline.multisample.sampleCount = 4u;
                    pipeline.depthStencil.format = rhi::Format::Undefined;
                    pipeline.depthStencil.depthTestEnable = false;
                    pipeline.depthStencil.depthWriteEnable = false;
                    pipeline.rasterizer.cullMode = rhi::CullMode::None;
                    PPR_TEST_ASSERT(
                        not make_error_code(device.createRenderPipeline(pipeline, m_probe_pipeline.writeRef())));
                    PPR_TEST_ASSERT(
                        not make_error_code(device.createRootShaderObject(m_probe_program, m_probe_root.writeRef())));

                    shader::ComPtr<slang::IEntryPoint> dump_entry;
                    PPR_TEST_ASSERT(not make_error_code(
                        module->findEntryPointByName("dumpReconstructionProbe", dump_entry.writeRef())));
                    slang::IComponentType *dump_components[] = {dump_entry.get()};
                    rhi::ShaderProgramDesc dump_description{};
                    dump_description.linkingStyle = rhi::LinkingStyle::SingleProgram;
                    dump_description.slangGlobalScope = module.get();
                    dump_description.slangEntryPoints = dump_components;
                    dump_description.slangEntryPointCount = 1u;
                    shader::Diagnose dump_diagnostics;
                    PPR_TEST_ASSERT(not make_error_code(device.createShaderProgram(
                        dump_description, m_dump_program.writeRef(), dump_diagnostics.writeRef())));
                    rhi::ComputePipelineDesc dump_pipeline{};
                    dump_pipeline.program = m_dump_program;
                    PPR_TEST_ASSERT(
                        not make_error_code(device.createComputePipeline(dump_pipeline, m_dump_pipeline.writeRef())));
                    PPR_TEST_ASSERT(
                        not make_error_code(device.createRootShaderObject(m_dump_program, m_dump_root.writeRef())));
                }

                FenceCompletionValue submit(rhi::ICommandEncoder &encoder, const bool wait) {
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
                    if (wait) {
                        PPR_TEST_ASSERT(not m_fence.waitFor(value));
                    }
                    m_last_value = value;
                    return value;
                }

                void renderScene(
                    const PresentationGrid &grid, const u32 mode, const float4 base, const bool wait = true) {
                    rhi::SubresourceRange range{.layer = 0u, .layerCount = 1u, .mip = 0u, .mipCount = 1u};
                    rhi::ComPtr<rhi::ITextureView> view = m_pass.sceneTexture().getDefaultView();
                    rhi::ComPtr<rhi::ICommandEncoder> encoder;
                    PPR_TEST_ASSERT(not make_error_code(m_queue->createCommandEncoder(encoder.writeRef())));
                    encoder->setTextureState(&m_pass.sceneTexture(), range, rhi::ResourceState::RenderTarget);

                    ProbeParameters parameters{};
                    parameters.m_extent = grid.m_raster;
                    parameters.m_mode = mode;
                    parameters.m_base = base;
                    rhi::ShaderCursor cursor{m_probe_root};
                    rhi::ShaderCursor constants;
                    PPR_TEST_ASSERT(not make_error_code(cursor["g_probe"].getDereferenced(constants)));
                    PPR_TEST_ASSERT(not make_error_code(constants.setData(&parameters, sizeof(parameters))));

                    const std::array<rhi::RenderPassColorAttachment, 1> colors{
                        rhi::RenderPassColorAttachment{.view = view, .clearValue = {0.0f, 0.0f, 0.0f, 0.0f}}};
                    rhi::RenderPassDesc pass_description{};
                    pass_description.colorAttachments = colors.data();
                    pass_description.colorAttachmentCount = 1u;
                    auto *const pass = encoder->beginRenderPass(pass_description);
                    PPR_TEST_ASSERT(pass != nullptr);
                    pass->setRenderState(
                        {.viewports = {rhi::Viewport::fromSize(
                             static_cast<float>(grid.m_raster.x), static_cast<float>(grid.m_raster.y))},
                            .viewportCount = 1u,
                            .scissorRects = {rhi::ScissorRect::fromSize(
                                static_cast<u32>(grid.m_raster.x), static_cast<u32>(grid.m_raster.y))},
                            .scissorRectCount = 1u,
                            .vertexBuffers = {},
                            .indexBuffer = {}});
                    pass->bindPipeline(m_probe_pipeline, m_probe_root);
                    pass->draw({.vertexCount = 3u});
                    pass->end();
                    encoder->setTextureState(&m_pass.sceneTexture(), range, rhi::ResourceState::ShaderResource);

                    const auto value = submit(*encoder, wait);
                    m_pass.notifySceneSubmitted(value);
                }

                struct FrameResult {
                    ResolvedHdrView m_view{};
                    FenceCompletionValue m_value{kFenceValueNone};
                };

                // Completes the stage-B half of a frame whose beginFrame has
                // already run. Used by commit-semantics and fence tests.
                FrameResult finishFrame(
                    const PresentationGrid &grid, const u32 mode, const float4 base, const bool wait = true) {
                    auto &device = m_rhi->getDevice();
                    renderScene(grid, mode, base, wait);

                    ResolvedHdrView resolved{};
                    rhi::ComPtr<rhi::ICommandEncoder> encoder;
                    PPR_TEST_ASSERT(not make_error_code(m_queue->createCommandEncoder(encoder.writeRef())));
                    const PreRenderContext context{device, *encoder, m_fence.completed(), {}};
                    PPR_TEST_ASSERT(not m_pass.prepare(context));
                    resolved = m_pass.resolved();
                    PPR_TEST_ASSERT(not m_pass.finalize(context));
                    const auto value = submit(*encoder, wait);
                    m_pass.notifySubmitted(value);
                    if (wait) {
                        PPR_TEST_ASSERT(not m_pass.collectCompleted(value));
                    }

                    return FrameResult{resolved, value};
                }

                FrameResult runFrame(
                    const CameraSnapshot &camera, const PresentationGrid &grid, const GridWorldIdentity world,
                    const u64 camera_cut, const u32 mode, const float4 base) {
                    auto &device = m_rhi->getDevice();
                    PPR_TEST_ASSERT(
                        not m_pass.beginFrame(device, camera, grid, world, camera_cut, m_fence.completed()));
                    return finishFrame(grid, mode, base);
                }

                [[nodiscard]] Readback readResolved(const ResolvedHdrView &view, const PresentationGrid &grid) {
                    rhi::ITexture *const texture = view.m_view->getTexture();
                    PPR_TEST_ASSERT(texture != nullptr);
                    shader::ComPtr<ISlangBlob> blob;
                    rhi::SubresourceLayout layout{};
                    PPR_TEST_ASSERT(
                        not make_error_code(m_rhi->getDevice().readTexture(texture, 0u, 0u, blob.writeRef(), &layout)));
                    const auto *const bytes = static_cast<const std::byte *>(blob->getBufferPointer());

                    Readback result{};
                    result.m_rgb.resize(
                        static_cast<std::size_t>(grid.m_backing.x) * static_cast<std::size_t>(grid.m_backing.y));
                    result.m_alpha = std::vector<float>(result.m_rgb.size(), 0.0f);
                    for (i32 y = 0; y < grid.m_backing.y; ++y) {
                        for (i32 x = 0; x < grid.m_backing.x; ++x) {
                            const auto *const texel = bytes +
                                                      static_cast<std::size_t>(y) * layout.rowPitch +
                                                      static_cast<std::size_t>(x) * 8u;
                            const auto channel = [&](const u32 index) {
                                u16 half{};
                                std::memcpy(&half, texel + index * sizeof(u16), sizeof(half));
                                return fromHalf(half);
                            };
                            const auto offset = static_cast<std::size_t>(y) *
                                                    static_cast<std::size_t>(grid.m_backing.x) +
                                                static_cast<std::size_t>(x);
                            result.m_rgb[offset] = float3{channel(0u), channel(1u), channel(2u)};
                            result.m_alpha[offset] = channel(3u);
                        }
                    }

                    return result;
                }

                // Test seam: rewrite the committed history texture with the same
                // valid-region bytes and arbitrary padding.
                void writeHistory(const ResolvedHdrView &view, const PresentationGrid &grid, const Readback &values) {
                    std::vector<u16> upload(
                        static_cast<std::size_t>(grid.m_backing.x) * static_cast<std::size_t>(grid.m_backing.y) * 4u);
                    for (i32 y = 0; y < grid.m_backing.y; ++y) {
                        for (i32 x = 0; x < grid.m_backing.x; ++x) {
                            const auto offset = static_cast<std::size_t>(y) *
                                                    static_cast<std::size_t>(grid.m_backing.x) +
                                                static_cast<std::size_t>(x);
                            const std::size_t texel = offset * 4u;
                            const bool inside = x < grid.m_resolved.x and y < grid.m_resolved.y;
                            const float3 color = inside ? values.m_rgb[offset] : float3{1000.0f, -1000.0f, 500.0f};
                            upload[texel + 0u] = toHalf(color.x);
                            upload[texel + 1u] = toHalf(color.y);
                            upload[texel + 2u] = toHalf(color.z);
                            upload[texel + 3u] = toHalf(inside ? 1.0f : 0.0f);
                        }
                    }

                    const rhi::SubresourceData initial{
                        .data = upload.data(),
                        .rowPitch = static_cast<std::size_t>(grid.m_backing.x) * 4u * sizeof(u16),
                        .slicePitch = upload.size() * sizeof(u16)};
                    rhi::TextureDesc description{};
                    description.type = rhi::TextureType::Texture2D;
                    description.size = {
                        .width = static_cast<u32>(grid.m_backing.x),
                        .height = static_cast<u32>(grid.m_backing.y),
                        .depth = 1u};
                    description.arrayLength = 1u;
                    description.mipCount = 1u;
                    description.format = rhi::Format::RGBA16Float;
                    description.usage = rhi::TextureUsage::CopySource;
                    description.defaultState = rhi::ResourceState::CopySource;
                    rhi::ComPtr<rhi::ITexture> source;
                    auto &device = m_rhi->getDevice();
                    PPR_TEST_ASSERT(
                        not make_error_code(device.createTexture(description, &initial, source.writeRef())));

                    rhi::ITexture *const destination = view.m_view->getTexture();
                    const rhi::SubresourceRange range{.layer = 0u, .layerCount = 1u, .mip = 0u, .mipCount = 1u};
                    rhi::ComPtr<rhi::ICommandEncoder> encoder;
                    PPR_TEST_ASSERT(not make_error_code(m_queue->createCommandEncoder(encoder.writeRef())));
                    encoder->setTextureState(destination, range, rhi::ResourceState::CopyDestination);
                    encoder->copyTexture(
                        destination, range, rhi::Offset3D{0u, 0u, 0u}, source, range, rhi::Offset3D{0u, 0u, 0u},
                        rhi::Extent3D{static_cast<u32>(grid.m_backing.x), static_cast<u32>(grid.m_backing.y), 1u});
                    encoder->setTextureState(destination, range, rhi::ResourceState::ShaderResource);
                    submit(*encoder, true);
                }

                [[nodiscard]] std::vector<float3> dumpScene(const PresentationGrid &grid) {
                    auto &device = m_rhi->getDevice();
                    const u32 width = static_cast<u32>(grid.m_raster.x) * 4u;
                    const u32 height = static_cast<u32>(grid.m_raster.y);
                    rhi::TextureDesc description{};
                    description.type = rhi::TextureType::Texture2D;
                    description.size = {.width = width, .height = height, .depth = 1u};
                    description.arrayLength = 1u;
                    description.mipCount = 1u;
                    description.format = rhi::Format::RGBA16Float;
                    description.usage = static_cast<rhi::TextureUsage>(
                        static_cast<u32>(rhi::TextureUsage::ShaderResource) |
                        static_cast<u32>(rhi::TextureUsage::UnorderedAccess) |
                        static_cast<u32>(rhi::TextureUsage::CopySource));
                    description.defaultState = rhi::ResourceState::UnorderedAccess;
                    rhi::ComPtr<rhi::ITexture> dump;
                    PPR_TEST_ASSERT(not make_error_code(device.createTexture(description, nullptr, dump.writeRef())));
                    rhi::ComPtr<rhi::ITextureView> dump_view = dump->getDefaultView();
                    rhi::ComPtr<rhi::ITextureView> scene_view = m_pass.sceneTexture().getDefaultView();

                    ProbeParameters parameters{};
                    parameters.m_extent = grid.m_raster;
                    rhi::ShaderCursor cursor{m_dump_root};
                    rhi::ShaderCursor constants;
                    PPR_TEST_ASSERT(not make_error_code(cursor["g_probe"].getDereferenced(constants)));
                    PPR_TEST_ASSERT(not make_error_code(constants.setData(&parameters, sizeof(parameters))));
                    PPR_TEST_ASSERT(not make_error_code(cursor["g_scene"].setBinding(rhi::Binding(scene_view))));
                    PPR_TEST_ASSERT(not make_error_code(cursor["g_dump"].setBinding(rhi::Binding(dump_view))));

                    const rhi::SubresourceRange range{.layer = 0u, .layerCount = 1u, .mip = 0u, .mipCount = 1u};
                    rhi::ComPtr<rhi::ICommandEncoder> encoder;
                    PPR_TEST_ASSERT(not make_error_code(m_queue->createCommandEncoder(encoder.writeRef())));
                    encoder->setTextureState(&m_pass.sceneTexture(), range, rhi::ResourceState::ShaderResource);
                    encoder->setTextureState(dump, range, rhi::ResourceState::UnorderedAccess);
                    auto *const pass = encoder->beginComputePass();
                    PPR_TEST_ASSERT(pass != nullptr);
                    pass->bindPipeline(m_dump_pipeline, m_dump_root);
                    pass->dispatchCompute(
                        (static_cast<u32>(grid.m_raster.x) + 7u) / 8u, (static_cast<u32>(grid.m_raster.y) + 7u) / 8u,
                        1u);
                    pass->end();
                    submit(*encoder, true);

                    shader::ComPtr<ISlangBlob> blob;
                    rhi::SubresourceLayout layout{};
                    PPR_TEST_ASSERT(not make_error_code(device.readTexture(dump, 0u, 0u, blob.writeRef(), &layout)));
                    const auto *const bytes = static_cast<const std::byte *>(blob->getBufferPointer());
                    std::vector<float3> samples(
                        static_cast<std::size_t>(grid.m_raster.x) * static_cast<std::size_t>(grid.m_raster.y) * 4u);
                    for (u32 y = 0u; y < height; ++y) {
                        for (u32 x = 0u; x < width; ++x) {
                            const auto *const texel = bytes +
                                                      static_cast<std::size_t>(y) * layout.rowPitch +
                                                      static_cast<std::size_t>(x) * 8u;
                            const auto channel = [&](const u32 index) {
                                u16 half{};
                                std::memcpy(&half, texel + index * sizeof(u16), sizeof(half));
                                return fromHalf(half);
                            };
                            const u32 source_x = x / 4u;
                            const u32 sample = x % 4u;
                            const std::size_t offset = (static_cast<std::size_t>(y) *
                                                               static_cast<std::size_t>(grid.m_raster.x) +
                                                           static_cast<std::size_t>(source_x)) *
                                                           4u +
                                                       sample;
                            samples[offset] = float3{channel(0u), channel(1u), channel(2u)};
                        }
                    }

                    return samples;
                }

                void shutdown() {
                    if (m_queue and make_error_code(m_queue->waitOnHost())) {
                        hal::fatalExit("reconstruction-test-quiescence-unknown");
                    }

                    PPR_TEST_ASSERT(not m_pass.shutdown());
                    PPR_TEST_ASSERT(not m_fence.shutdown());
                    m_probe_root.setNull();
                    m_probe_pipeline.setNull();
                    m_probe_program.setNull();
                    m_dump_root.setNull();
                    m_dump_pipeline.setNull();
                    m_dump_program.setNull();
                    m_queue.setNull();
                    PPR_TEST_ASSERT(not m_rhi->shutdown());
                    PPR_TEST_ASSERT(not m_shaders->shutdown());
                }
            };

            [[nodiscard]] GridWorldIdentity worldIdentity(const u64 generation) noexcept {
                return GridWorldIdentity{
                    .m_job_id = 1u, .m_world_generation = generation, .m_content_fingerprint = generation};
            }

            [[nodiscard]] float2 phaseJitter(const u32 phase) noexcept {
                return float2{0.5f * static_cast<float>(phase & 1u), 0.5f * static_cast<float>((phase >> 1u) & 1u)};
            }

            // Interior comparison band: the first and last resolved row/column
            // are excluded from history-blend comparisons because a stationary
            // bilinear coordinate there can land on either side of a texel
            // centre by a few ULPs, which the literal nonzero-weight rule is
            // meant to treat differently. Reset/current-only and dedicated
            // padding cases still compare those edges.
            [[nodiscard]] bool interiorBlendPixel(const i32 x, const i32 y, const PresentationGrid &grid) noexcept {
                return x > 0 and y > 0 and x + 1 < grid.m_resolved.x and y + 1 < grid.m_resolved.y;
            }

            void assertClose(const float3 actual, const float3 expected, const float tolerance) {
                PPR_TEST_ASSERT(std::abs(actual.x - expected.x) <= tolerance);
                PPR_TEST_ASSERT(std::abs(actual.y - expected.y) <= tolerance);
                PPR_TEST_ASSERT(std::abs(actual.z - expected.z) <= tolerance);
            }
        } // namespace Gpu

        // ------------------------------------------------------------------
        // CPU: checked extents, lattice and history oracle
        // ------------------------------------------------------------------

        PPR_UNIT_TEST (presentation_extents_and_caller_suspension) {
            const auto odd_1919 = makePresentationGrid(int2{1919, 1079}, 2u);
            PPR_TEST_ASSERT(odd_1919.has_value());
            PPR_TEST_ASSERT(same(odd_1919->m_resolved, int2(960, 540)));
            PPR_TEST_ASSERT(same(odd_1919->m_raster, int2(480, 270)));
            PPR_TEST_ASSERT(same(odd_1919->m_backing, int2(960, 540)));

            const auto odd_1921 = makePresentationGrid(int2{1921, 1081}, 2u);
            PPR_TEST_ASSERT(odd_1921.has_value());
            PPR_TEST_ASSERT(same(odd_1921->m_resolved, int2(961, 541)));
            PPR_TEST_ASSERT(same(odd_1921->m_raster, int2(481, 271)));
            PPR_TEST_ASSERT(same(odd_1921->m_backing, int2(962, 542)));

            const auto single = makePresentationGrid(int2{1, 1}, 1u);
            PPR_TEST_ASSERT(single.has_value());
            PPR_TEST_ASSERT(
                same(single->m_resolved, int2(1, 1)) and
                same(single->m_raster, int2(1, 1)) and
                same(single->m_backing, int2(2, 2)));

            // Zero framebuffer extents are caller suspension, never a fabricated
            // 1x1 picture.
            for (const int2 &rejected: {int2{0, 0}, int2{0, 24}, int2{24, 0}, int2{-1, 24}}) {
                const auto grid = makePresentationGrid(rejected, 1u);
                PPR_TEST_ASSERT(not grid and grid.error() == std::errc::invalid_argument);
            }

            const auto overflow = makePresentationGrid(int2{std::numeric_limits<i32>::max(), 1}, 1u);
            PPR_TEST_ASSERT(not overflow and overflow.error() == std::errc::value_too_large);
        };

        PPR_UNIT_TEST (two_resolution_projection_alignment) {
            // The same visible window pixel must map to the same visible NDC at
            // every pixelization factor: q = window_pixel/P and
            // ndc = 2*P*q/W - 1. This is the reconstruction-side half of the
            // layered-background alignment obligation; the scene/background
            // draw path is Task 14 integration.
            for (const int2 &window: {int2{1919, 1079}, int2{1921, 1081}, int2{1920, 1080}}) {
                for (const u32 scale: {1u, 2u, 3u}) {
                    const auto grid = makePresentationGrid(window, scale);
                    PPR_TEST_ASSERT(grid.has_value());
                    for (const float visible_x:
                        {0.5f, static_cast<float>(window.x) * 0.25f + 0.5f, static_cast<float>(window.x) * 0.5f + 0.5f,
                            static_cast<float>(window.x) - 0.5f}) {
                        const float resolved_x = visible_x / static_cast<float>(scale);
                        PPR_TEST_ASSERT(resolved_x >= 0.0f and resolved_x <= static_cast<float>(grid->m_resolved.x));
                        const float ndc = 2.0f * static_cast<float>(scale) * resolved_x / static_cast<float>(window.x) -
                                          1.0f;
                        const float expected = 2.0f * visible_x / static_cast<float>(window.x) - 1.0f;
                        PPR_TEST_ASSERT(std::abs(ndc - expected) <= 1.0e-6f);
                    }
                    for (const float visible_y:
                        {0.5f, static_cast<float>(window.y) * 0.5f + 0.5f, static_cast<float>(window.y) - 0.5f}) {
                        const float resolved_y = visible_y / static_cast<float>(scale);
                        const float ndc = 1.0f -
                                          2.0f * static_cast<float>(scale) * resolved_y / static_cast<float>(window.y);
                        const float expected = 1.0f - 2.0f * visible_y / static_cast<float>(window.y);
                        PPR_TEST_ASSERT(std::abs(ndc - expected) <= 1.0e-6f);
                    }
                }
            }
        };

        PPR_UNIT_TEST (lattice_known_nodes_and_signed_floor_division) {
            using namespace Oracle;

            PPR_TEST_ASSERT(floorDiv(-1, 4) == -1 and floorMod(-1, 4) == 3);
            PPR_TEST_ASSERT(floorDiv(-5, 4) == -2 and floorMod(-5, 4) == 3);
            PPR_TEST_ASSERT(floorDiv(5, 4) == 1 and floorMod(5, 4) == 1);
            PPR_TEST_ASSERT(floorDiv(-4, 4) == -1 and floorMod(-4, 4) == 0);

            const int2 raster{4, 4};
            const int2 resolved{8, 8};
            const float2 jitter{0.0f, 0.0f};
            const std::array<int2, 4> first_block{int2{1, 0}, int2{3, 1}, int2{0, 2}, int2{2, 3}};
            for (u32 sample = 0u; sample < first_block.size(); ++sample) {
                const auto mapped = resolveKnownNode(first_block[sample], raster, resolved, jitter);
                PPR_TEST_ASSERT(mapped.has_value());
                PPR_TEST_ASSERT(same(mapped->m_pixel, int2(0, 0)));
                PPR_TEST_ASSERT(mapped->m_index == sample);
            }

            const std::array<int2, 4> second_block{int2{5, 4}, int2{7, 5}, int2{4, 6}, int2{6, 7}};
            for (u32 sample = 0u; sample < second_block.size(); ++sample) {
                const auto mapped = resolveKnownNode(second_block[sample], raster, resolved, jitter);
                PPR_TEST_ASSERT(mapped.has_value());
                PPR_TEST_ASSERT(same(mapped->m_pixel, int2(1, 1)));
                PPR_TEST_ASSERT(mapped->m_index == sample);
            }

            PPR_TEST_ASSERT(knownIndex(int2{0, 0}) == 4u);
            PPR_TEST_ASSERT(knownIndex(int2{8, 8}) == 4u);

            // A known node in the first negative block uses signed floor division;
            // the requested pixel is outside the source and replicates to the
            // nearest valid sample.
            const auto negative = resolveValidSample(int2{-1, 0}, 0u, int2{2, 2}, int2{4, 4}, jitter);
            PPR_TEST_ASSERT(negative.has_value());
            PPR_TEST_ASSERT(same(negative->m_pixel, int2(0, 0)));
            PPR_TEST_ASSERT(negative->m_index == 2u);
        };

        PPR_UNIT_TEST (lattice_diamond_interpolation_and_original_sample_bounds) {
            using namespace Oracle;

            const int2 raster{8, 8};
            const int2 resolved{16, 16};
            const float2 jitter{0.0f, 0.0f};
            std::vector<float3> samples(
                static_cast<std::size_t>(raster.x) * static_cast<std::size_t>(raster.y) * 4u, float3{0.0f, 0.0f, 0.0f});
            samples[sampleIndex(raster, int2{1, 2}, 0u)] = float3{1.0f, 0.0f, 0.0f};
            samples[sampleIndex(raster, int2{2, 2}, 0u)] = float3{0.0f, 1.0f, 0.0f};
            samples[sampleIndex(raster, int2{2, 1}, 2u)] = float3{0.0f, 0.0f, 1.0f};
            samples[sampleIndex(raster, int2{2, 2}, 2u)] = float3{0.5f, 0.5f, 0.5f};

            const auto node = reconstructNode(int2{8, 8}, samples, raster, resolved, jitter);
            PPR_TEST_ASSERT(node.has_value());
            // dh = dv = 0.25, so wh = 0.5 and the result is the four-sample mean.
            assertClose(node->m_color, float3{0.375f, 0.375f, 0.375f}, 1.0e-6f);
            assertClose(node->m_minimum, float3{0.0f, 0.0f, 0.0f}, 1.0e-6f);
            assertClose(node->m_maximum, float3{1.0f, 1.0f, 1.0f}, 1.0e-6f);

            // A directly known node contributes only its own sample.
            samples[sampleIndex(raster, int2{1, 1}, 0u)] = float3{0.25f, 0.5f, 0.75f};
            const auto known = reconstructNode(int2{5, 4}, samples, raster, resolved, jitter);
            PPR_TEST_ASSERT(known.has_value());
            assertClose(known->m_color, float3{0.25f, 0.5f, 0.75f}, 1.0e-6f);
            assertClose(known->m_minimum, known->m_color, 1.0e-6f);
            assertClose(known->m_maximum, known->m_color, 1.0e-6f);
        };

        PPR_UNIT_TEST (lattice_boundary_replication_and_tie_order) {
            using namespace Oracle;

            // Two equally near valid samples at stationary distance sqrt(3.25):
            // row-major source pixel then sample index chooses (0,0), sample 3.
            const auto tie = nearestValidSample(int2{1, 1}, 2u, int2{2, 1}, int2{3, 2}, float2{0.5f, 0.0f});
            PPR_TEST_ASSERT(tie.has_value());
            PPR_TEST_ASSERT(same(tie->m_pixel, int2(0, 0)));
            PPR_TEST_ASSERT(tie->m_index == 3u);

            // R=1 has a valid candidate for every requested sample and phase.
            for (u32 phase = 0u; phase < 4u; ++phase) {
                const float2 jitter = Gpu::phaseJitter(phase);
                for (u32 sample = 0u; sample < 4u; ++sample) {
                    for (i32 y = -1; y <= 1; ++y) {
                        for (i32 x = -1; x <= 1; ++x) {
                            const auto resolved = nearestValidSample(
                                int2{x, y}, sample, int2{1, 1}, int2{1, 1}, jitter);
                            PPR_TEST_ASSERT(resolved.has_value());
                            PPR_TEST_ASSERT(same(resolved->m_pixel, int2(0, 0)));
                        }
                    }
                }
            }
        };

        PPR_UNIT_TEST (lattice_stationary_positions_do_not_drift) {
            using namespace Oracle;

            for (u32 phase = 0u; phase < 4u; ++phase) {
                const float2 jitter = Gpu::phaseJitter(phase);
                const int2 shift{static_cast<i32>(4.0f * jitter.x), static_cast<i32>(4.0f * jitter.y)};
                for (const int2 &pixel: {int2{0, 0}, int2{3, 5}, int2{11, 7}}) {
                    for (i32 dy = 0; dy < 2; ++dy) {
                        for (i32 dx = 0; dx < 2; ++dx) {
                            const int2 node{2 * pixel.x + dx + shift.x, 2 * pixel.y + dy + shift.y};
                            const float2 stationary{
                                (static_cast<float>(node.x) + 0.5f) / 4.0f - jitter.x,
                                (static_cast<float>(node.y) + 0.5f) / 4.0f - jitter.y};
                            const float2 expected{
                                (static_cast<float>(2 * pixel.x + dx) + 0.5f) / 4.0f,
                                (static_cast<float>(2 * pixel.y + dy) + 0.5f) / 4.0f};
                            PPR_TEST_ASSERT(std::abs(stationary.x - expected.x) <= 1.0e-6f);
                            PPR_TEST_ASSERT(std::abs(stationary.y - expected.y) <= 1.0e-6f);
                        }
                    }
                }
            }
        };

        PPR_UNIT_TEST (motion_weight_curve) {
            using namespace Oracle;
            PPR_TEST_ASSERT(std::abs(currentWeight(0.0f) - 0.05f) <= 1.0e-6f);
            PPR_TEST_ASSERT(std::abs(currentWeight(0.1f) - 0.05f) <= 1.0e-6f);
            PPR_TEST_ASSERT(std::abs(currentWeight(0.3f) - 0.15f) <= 1.0e-6f);
            PPR_TEST_ASSERT(std::abs(currentWeight(0.5f) - 0.25f) <= 1.0e-6f);
            PPR_TEST_ASSERT(std::abs(currentWeight(2.0f) - 0.25f) <= 1.0e-6f);
        };

        PPR_UNIT_TEST (lattice_geometric_scan_brackets_unknown_nodes) {
            using namespace Oracle;

            const int2 raster{16, 16};
            const int2 resolved{32, 32};
            const float2 jitter{0.0f, 0.0f};
            u32 checked = 0u;
            for (i32 y = -9; y <= 9; ++y) {
                for (i32 x = -9; x <= 9; ++x) {
                    const int2 node{x, y};
                    if (knownIndex(node) != 4u) {
                        continue;
                    }
                    ++checked;

                    const auto left = findNearestKnown(node, int2{-1, 0}, raster, resolved, jitter);
                    const auto right = findNearestKnown(node, int2{1, 0}, raster, resolved, jitter);
                    const auto up = findNearestKnown(node, int2{0, -1}, raster, resolved, jitter);
                    const auto down = findNearestKnown(node, int2{0, 1}, raster, resolved, jitter);
                    PPR_TEST_ASSERT(left and right and up and down);
                    PPR_TEST_ASSERT(left->m_node.y == node.y and left->m_node.x < node.x);
                    PPR_TEST_ASSERT(right->m_node.y == node.y and right->m_node.x > node.x);
                    PPR_TEST_ASSERT(up->m_node.x == node.x and up->m_node.y < node.y);
                    PPR_TEST_ASSERT(down->m_node.x == node.x and down->m_node.y > node.y);

                    // No known node may lie strictly between the bracket and the
                    // node; the scan is the nearest, not merely any known node.
                    for (i32 between = left->m_node.x + 1; between < node.x; ++between) {
                        PPR_TEST_ASSERT(knownIndex(int2{between, node.y}) == 4u);
                    }
                    for (i32 between = node.x + 1; between < right->m_node.x; ++between) {
                        PPR_TEST_ASSERT(knownIndex(int2{between, node.y}) == 4u);
                    }
                    for (i32 between = up->m_node.y + 1; between < node.y; ++between) {
                        PPR_TEST_ASSERT(knownIndex(int2{node.x, between}) == 4u);
                    }
                    for (i32 between = node.y + 1; between < down->m_node.y; ++between) {
                        PPR_TEST_ASSERT(knownIndex(int2{node.x, between}) == 4u);
                    }
                }
            }
            PPR_TEST_ASSERT(checked > 100u);
        };

        PPR_UNIT_TEST (history_footprint_and_poison_padding_excluded) {
            using namespace Oracle;

            const int2 backing{4, 4};
            const int2 resolved{3, 3};
            std::vector<float3> history(
                static_cast<std::size_t>(backing.x) * static_cast<std::size_t>(backing.y), float3{0.0f, 0.0f, 0.0f});
            for (i32 y = 0; y < resolved.y; ++y) {
                for (i32 x = 0; x < resolved.x; ++x) {
                    history
                        [static_cast<std::size_t>(y) * static_cast<std::size_t>(backing.x) +
                            static_cast<std::size_t>(x)] = float3{0.25f, 0.5f, 0.75f};
                }
            }
            for (i32 y = 0; y < backing.y; ++y) {
                history[static_cast<std::size_t>(y) * static_cast<std::size_t>(backing.x) + 3u] = float3{
                    1000.0f, -1000.0f, 500.0f};
            }

            // A texel-centre sample reads only the valid region; poisoned padding
            // cannot contribute.
            const auto interior = sampleHistory(history, backing, resolved, float2{1.5f, 1.5f});
            PPR_TEST_ASSERT(interior.has_value());
            assertClose(*interior, float3{0.25f, 0.5f, 0.75f}, 1.0e-6f);

            // A nonzero-weight footprint that wants the padding is rejected, not
            // clamped to the edge.
            PPR_TEST_ASSERT(not sampleHistory(history, backing, resolved, float2{2.6f, 1.5f}).has_value());
            PPR_TEST_ASSERT(not sampleHistory(history, backing, resolved, float2{-10.0f, -10.0f}).has_value());
        };

        PPR_UNIT_TEST (history_requires_every_nonzero_weight_texel_inside_r) {
            using namespace Oracle;

            const int2 backing{4, 4};
            const int2 resolved{4, 4};
            std::vector<float3> history(
                static_cast<std::size_t>(backing.x) * static_cast<std::size_t>(backing.y), float3{0.25f, 0.5f, 0.75f});

            // Exactly on a texel centre the neighbour weight is exactly zero; it
            // is ignored and the sample is valid.
            const auto exact = sampleHistory(history, backing, resolved, float2{3.5f, 1.5f});
            PPR_TEST_ASSERT(exact.has_value());
            assertClose(*exact, float3{0.25f, 0.5f, 0.75f}, 1.0e-6f);

            // A 5e-5 nonzero weight on a padded texel outside R must invalidate
            // the history sample; there is no weight floor.
            PPR_TEST_ASSERT(not sampleHistory(history, backing, resolved, float2{3.50005f, 1.5f}).has_value());
        };

        // ------------------------------------------------------------------
        // GPU: production pass against the independent oracle
        // ------------------------------------------------------------------

        PPR_UNIT_TEST (gpu_exact_extents_projection_and_caller_suspension) {
            Gpu::Fixture fixture;
            fixture.initialize();
            PPR_DEFER {
                fixture.shutdown();
            };

            const int2 window{1919, 1079};
            const auto grid = makePresentationGrid(window, 2u);
            PPR_TEST_ASSERT(grid.has_value());
            const auto camera = makeOrthoCamera(window, float3{0.0f, 0.0f, -1.0f});
            const auto world = Gpu::worldIdentity(1u);

            PPR_TEST_ASSERT(not fixture.m_pass.beginFrame(
                fixture.m_rhi->getDevice(), camera, *grid, world, 0u, fixture.m_fence.completed()));
            const rhi::TextureDesc &scene = fixture.m_pass.sceneTexture().getDesc();
            PPR_TEST_ASSERT(scene.format == rhi::Format::RGBA16Float);
            PPR_TEST_ASSERT(scene.sampleCount == 4u);
            PPR_TEST_ASSERT(scene.size.width == 480u and scene.size.height == 270u);
            PPR_TEST_ASSERT(same(fixture.m_pass.sceneCamera().m_viewport_size, float2{480.0f, 270.0f}));
            PPR_TEST_ASSERT(same(fixture.m_pass.sceneCamera().m_jitter, float2{0.0f, 0.0f}));

            // Padding extends right/bottom: compose the same world→clip mapping
            // with the approved clip-space padding transform.
            const float ax = static_cast<float>(window.x) / (2.0f * 960.0f);
            const float ay = static_cast<float>(window.y) / (2.0f * 540.0f);
            float4x4 pad = float4x4::identity();
            pad[0][0] = ax;
            pad[1][1] = ay;
            pad[3][0] = ax - 1.0f;
            pad[3][1] = 1.0f - ay;
            const std::array<float4, 3> world_points{
                float4{0.0f, 0.0f, 0.0f, 1.0f},
                float4{static_cast<float>(window.x), static_cast<float>(window.y), 0.0f, 1.0f},
                float4{static_cast<float>(window.x) * 0.5f, static_cast<float>(window.y) * 0.5f, 0.0f, 1.0f}};
            for (const float4 &point: world_points) {
                const float4 actual = point * fixture.m_pass.sceneCamera().m_projection;
                const float4 expected = (point * camera.m_projection) * pad;
                for (u32 component = 0u; component < 4u; ++component) {
                    PPR_TEST_ASSERT(std::abs(actual[component] - expected[component]) <= 1.0e-5f);
                }
            }

            const float4 left = (world_points[0] * camera.m_projection) * pad;
            const float4 right = (world_points[1] * camera.m_projection) * pad;
            const float4 centre = (world_points[2] * camera.m_projection) * pad;
            PPR_TEST_ASSERT(std::abs(left[0] + 1.0f) <= 1.0e-5f);
            PPR_TEST_ASSERT(std::abs(left[1] - (1.0f - 2.0f * ay)) <= 1.0e-5f);
            PPR_TEST_ASSERT(std::abs(right[0] - (2.0f * ax - 1.0f)) <= 1.0e-5f);
            PPR_TEST_ASSERT(std::abs(right[1] - 1.0f) <= 1.0e-5f);
            PPR_TEST_ASSERT(std::abs(centre[0] - (ax - 1.0f)) <= 1.0e-5f);
            PPR_TEST_ASSERT(std::abs(centre[1] - (1.0f - ay)) <= 1.0e-5f);

            fixture.m_pass.discardUnsubmitted();

            // All four phases on the smallest legal grid (R=1, S=1, B=2).
            const auto single = makePresentationGrid(int2{1, 1}, 1u);
            PPR_TEST_ASSERT(single.has_value());
            const auto single_camera = makeOrthoCamera(int2{1, 1}, float3{0.0f, 0.0f, -1.0f});
            for (u32 phase = 0u; phase < 4u; ++phase) {
                PPR_TEST_ASSERT(not fixture.m_pass.beginFrame(
                    fixture.m_rhi->getDevice(), single_camera, *single, world, 0u, fixture.m_fence.completed()));
                const float2 jitter = Gpu::phaseJitter(phase);
                PPR_TEST_ASSERT(same(
                    fixture.m_pass.sceneCamera().m_jitter, float2{2.0f * jitter.x / 1.0f, -2.0f * jitter.y / 1.0f}));
                const auto result = fixture.finishFrame(*single, 1u, float4{0.5f, 0.5f, 0.5f, 1.0f});
                PPR_TEST_ASSERT(same(result.m_view.m_valid, int2(1, 1)));
                PPR_TEST_ASSERT(same(result.m_view.m_backing, int2(2, 2)));
                const auto pixels = fixture.readResolved(result.m_view, *single);
                PPR_TEST_ASSERT(pixels.m_rgb.size() == 4u);
                PPR_TEST_ASSERT(pixels.m_alpha[0] > 0.5f);
                PPR_TEST_ASSERT(pixels.m_alpha[3] < 0.5f);
            }

            // Zero-framebuffer suspension is never a fabricated extent.
            PresentationGrid zero{};
            PPR_TEST_ASSERT(
                fixture.m_pass.beginFrame(
                    fixture.m_rhi->getDevice(), single_camera, zero, world, 0u, fixture.m_fence.completed()) ==
                std::errc::invalid_argument);
        };

        PPR_UNIT_TEST (gpu_lattice_matches_independent_oracle_all_phases) {
            Gpu::Fixture fixture;
            fixture.initialize();
            PPR_DEFER {
                fixture.shutdown();
            };

            const int2 window{37, 21};
            const auto grid = makePresentationGrid(window, 2u);
            PPR_TEST_ASSERT(grid.has_value());
            const auto camera = makeOrthoCamera(window, float3{0.0f, 0.0f, -1.0f});
            const auto world = Gpu::worldIdentity(3u);
            const auto samples = Oracle::makeProbeSamples(grid->m_raster, 1.0f);

            std::vector<float3> previous_history;
            std::vector<float3> previous_current;
            bool history_valid = false;
            CameraSnapshot previous_camera{};

            for (u32 phase = 0u; phase < 4u; ++phase) {
                const auto result = fixture.runFrame(camera, *grid, world, 0u, 0u, float4{0.0f, 0.0f, 0.0f, 1.0f});
                const auto expected = Oracle::expectedFrame(
                    samples, *grid, camera, phase, history_valid, previous_history, *grid, previous_camera);
                const auto actual = fixture.readResolved(result.m_view, *grid);
                for (i32 y = 0; y < grid->m_resolved.y; ++y) {
                    for (i32 x = 0; x < grid->m_resolved.x; ++x) {
                        const auto offset = static_cast<std::size_t>(y) * static_cast<std::size_t>(grid->m_backing.x) +
                                            static_cast<std::size_t>(x);
                        if (not history_valid or Gpu::interiorBlendPixel(x, y, *grid)) {
                            Gpu::assertClose(actual.m_rgb[offset], expected[offset], 2.0e-3f);
                        }
                        PPR_TEST_ASSERT(actual.m_alpha[offset] > 0.5f);
                    }
                }

                previous_history = actual.m_rgb;
                history_valid = true;
                previous_camera = camera;
            }
        };

        PPR_UNIT_TEST (gpu_probe_samples_match_cpu_pattern) {
            Gpu::Fixture fixture;
            fixture.initialize();
            PPR_DEFER {
                fixture.shutdown();
            };

            const int2 window{37, 21};
            const auto grid = makePresentationGrid(window, 2u);
            PPR_TEST_ASSERT(grid.has_value());
            const auto world = Gpu::worldIdentity(11u);
            const auto camera = makeOrthoCamera(window, float3{0.0f, 0.0f, -1.0f});
            PPR_TEST_ASSERT(not fixture.m_pass.beginFrame(
                fixture.m_rhi->getDevice(), camera, *grid, world, 0u, fixture.m_fence.completed()));
            fixture.renderScene(*grid, 0u, float4{0.0f, 0.0f, 0.0f, 1.0f});
            const auto actual = fixture.dumpScene(*grid);
            fixture.m_pass.discardUnsubmitted();

            const auto expected = Oracle::makeProbeSamples(grid->m_raster, 1.0f);
            PPR_TEST_ASSERT(actual.size() == expected.size());
            for (std::size_t index = 0u; index < actual.size(); ++index) {
                Gpu::assertClose(actual[index], expected[index], 1.0e-3f);
            }
        };

        PPR_UNIT_TEST (gpu_constant_color_every_phase) {
            Gpu::Fixture fixture;
            fixture.initialize();
            PPR_DEFER {
                fixture.shutdown();
            };

            const int2 window{23, 15};
            const auto grid = makePresentationGrid(window, 1u);
            PPR_TEST_ASSERT(grid.has_value());
            const auto camera = makeOrthoCamera(window, float3{0.0f, 0.0f, -1.0f});
            const auto world = Gpu::worldIdentity(4u);
            const float3 constant{0.25f, 0.5f, 0.75f};

            for (u32 phase = 0u; phase < 4u; ++phase) {
                const auto result = fixture.runFrame(
                    camera, *grid, world, 0u, 1u, float4{constant.x, constant.y, constant.z, 1.0f});
                const auto actual = fixture.readResolved(result.m_view, *grid);
                for (i32 y = 0; y < grid->m_resolved.y; ++y) {
                    for (i32 x = 0; x < grid->m_resolved.x; ++x) {
                        const auto offset = static_cast<std::size_t>(y) * static_cast<std::size_t>(grid->m_backing.x) +
                                            static_cast<std::size_t>(x);
                        Gpu::assertClose(actual.m_rgb[offset], constant, 1.0e-3f);
                    }
                }
            }
        };

        PPR_UNIT_TEST (gpu_temporal_motion_and_offscreen_history) {
            Gpu::Fixture fixture;
            fixture.initialize();
            PPR_DEFER {
                fixture.shutdown();
            };

            const int2 window{33, 17};
            const auto grid = makePresentationGrid(window, 1u);
            PPR_TEST_ASSERT(grid.has_value());
            const auto world = Gpu::worldIdentity(5u);
            const auto samples = Oracle::makeProbeSamples(grid->m_raster, 1.0f);

            float3 origin{0.0f, 0.0f, -1.0f};
            auto camera = makeOrthoCamera(window, origin);
            const auto first = fixture.runFrame(camera, *grid, world, 0u, 0u, float4{0.0f, 0.0f, 0.0f, 1.0f});
            auto previous_history = fixture.readResolved(first.m_view, *grid).m_rgb;
            auto previous_camera = camera;

            const std::array<float, 5> motions{0.0f, 0.1f, 0.3f, 0.5f, 40.0f};
            for (u32 index = 0u; index < motions.size(); ++index) {
                const float motion = motions[index];
                origin.x -= motion;
                camera = makeOrthoCamera(window, origin);
                const float tint = 0.9f - 0.1f * static_cast<float>(index);
                const u32 frame_phase = (index + 1u) % 4u;
                const auto result = fixture.runFrame(camera, *grid, world, 0u, 0u, float4{0.0f, 0.0f, 0.0f, tint});
                const auto expected = Oracle::expectedFrame(
                    samples, *grid, camera, frame_phase, true, previous_history, *grid, previous_camera, tint);
                const auto current_only = Oracle::expectedFrame(
                    samples, *grid, camera, frame_phase, false, previous_history, *grid, previous_camera, tint);
                const auto actual = fixture.readResolved(result.m_view, *grid);

                u32 blended = 0u;
                for (i32 y = 0; y < grid->m_resolved.y; ++y) {
                    for (i32 x = 0; x < grid->m_resolved.x; ++x) {
                        const auto offset = static_cast<std::size_t>(y) * static_cast<std::size_t>(grid->m_backing.x) +
                                            static_cast<std::size_t>(x);
                        if (motion >= 1.0f or Gpu::interiorBlendPixel(x, y, *grid)) {
                            Gpu::assertClose(actual.m_rgb[offset], expected[offset], 3.0e-3f);
                        }
                        const float3 delta = expected[offset] - current_only[offset];
                        if (std::abs(delta.x) + std::abs(delta.y) + std::abs(delta.z) > 1.0e-4f) {
                            ++blended;
                        }
                    }
                }

                if (motion >= 0.1f and motion < 1.0f) {
                    PPR_TEST_ASSERT(blended > 0u);
                }

                if (motion >= 1.0f) {
                    // Large motion leaves the valid history rectangle: every pixel
                    // is current-only, never a clamped edge sample.
                    for (i32 y = 0; y < grid->m_resolved.y; ++y) {
                        for (i32 x = 0; x < grid->m_resolved.x; ++x) {
                            const auto offset = static_cast<std::size_t>(y) *
                                                    static_cast<std::size_t>(grid->m_backing.x) +
                                                static_cast<std::size_t>(x);
                            Gpu::assertClose(actual.m_rgb[offset], current_only[offset], 3.0e-3f);
                        }
                    }
                }

                previous_history = actual.m_rgb;
                previous_camera = camera;
            }
        };

        PPR_UNIT_TEST (gpu_padding_history_is_never_read) {
            Gpu::Fixture fixture;
            fixture.initialize();
            PPR_DEFER {
                fixture.shutdown();
            };

            // R=3, S=2, B=4: the resolved backing carries one padded column.
            const int2 window{5, 5};
            const auto grid = makePresentationGrid(window, 2u);
            PPR_TEST_ASSERT(grid.has_value());
            PPR_TEST_ASSERT(same(grid->m_resolved, int2(3, 3)));
            PPR_TEST_ASSERT(same(grid->m_backing, int2(4, 4)));
            const auto world = Gpu::worldIdentity(6u);
            const auto samples = Oracle::makeProbeSamples(grid->m_raster, 1.0f);

            const auto first_camera = makeOrthoCamera(window, float3{-0.4f, 0.0f, -1.0f});
            const auto first = fixture.runFrame(first_camera, *grid, world, 0u, 0u, float4{0.0f, 0.0f, 0.0f, 1.0f});
            const auto previous = fixture.readResolved(first.m_view, *grid);
            fixture.writeHistory(first.m_view, *grid, previous);

            const auto camera = makeOrthoCamera(window, float3{0.0f, 0.0f, -1.0f});
            const auto result = fixture.runFrame(camera, *grid, world, 0u, 0u, float4{0.0f, 0.0f, 0.0f, 0.8f});
            const auto expected = Oracle::expectedFrame(
                samples, *grid, camera, 1u, true, previous.m_rgb, *grid, first_camera, 0.8f);
            const auto current_only = Oracle::expectedFrame(
                samples, *grid, camera, 1u, false, previous.m_rgb, *grid, first_camera, 0.8f);
            const auto actual = fixture.readResolved(result.m_view, *grid);

            for (i32 y = 0; y < grid->m_resolved.y; ++y) {
                for (i32 x = 0; x < grid->m_resolved.x; ++x) {
                    const auto offset = static_cast<std::size_t>(y) * static_cast<std::size_t>(grid->m_backing.x) +
                                        static_cast<std::size_t>(x);
                    if (Gpu::interiorBlendPixel(x, y, *grid)) {
                        Gpu::assertClose(actual.m_rgb[offset], expected[offset], 3.0e-3f);
                    }
                }
            }

            // The rightmost column wants a nonzero padded texel and falls back
            // to the current reconstruction instead of clamped poison.
            for (i32 y = 0; y < grid->m_resolved.y; ++y) {
                const auto offset = static_cast<std::size_t>(y) * static_cast<std::size_t>(grid->m_backing.x) +
                                    static_cast<std::size_t>(grid->m_resolved.x - 1);
                if (y > 0 and y + 1 < grid->m_resolved.y) {
                    Gpu::assertClose(actual.m_rgb[offset], current_only[offset], 3.0e-3f);
                }
                PPR_TEST_ASSERT(actual.m_rgb[offset].x < 100.0f);
            }
        };

        PPR_UNIT_TEST (gpu_history_reset_and_preserve_matrix) {
            Gpu::Fixture fixture;
            fixture.initialize();
            PPR_DEFER {
                fixture.shutdown();
            };

            const int2 window{21, 13};
            const auto grid0 = makePresentationGrid(window, 2u);
            PPR_TEST_ASSERT(grid0.has_value());
            const auto grid1 = makePresentationGrid(int2{23, 15}, 2u);
            PPR_TEST_ASSERT(grid1.has_value());
            const auto world0 = Gpu::worldIdentity(7u);
            const auto world1 = Gpu::worldIdentity(8u);
            CameraSnapshot camera = makeOrthoCamera(window, float3{0.0f, 0.0f, -1.0f});

            const auto first = fixture.runFrame(camera, *grid0, world0, 0u, 0u, float4{0.0f, 0.0f, 0.0f, 1.0f});
            auto previous = fixture.readResolved(first.m_view, *grid0);
            auto previous_camera = camera;
            auto previous_grid = *grid0;
            auto previous_world = world0;
            u64 previous_cut = 0u;
            u32 committed_phase = 0u;

            const auto check = [&](const PresentationGrid &grid, const CameraSnapshot &frame_camera,
                                   const GridWorldIdentity world, const u64 cut, const float tint) {
                const bool reset = previous_world != world or previous_cut != cut or not (previous_grid == grid);
                if (reset) {
                    committed_phase = 0u;
                } else {
                    committed_phase = (committed_phase + 1u) % 4u;
                }

                const auto samples = Oracle::makeProbeSamples(grid.m_raster, 1.0f);
                const auto result = fixture.runFrame(
                    frame_camera, grid, world, cut, 0u, float4{0.0f, 0.0f, 0.0f, tint});
                const auto expected = Oracle::expectedFrame(
                    samples, grid, frame_camera, committed_phase, not reset, previous.m_rgb, previous_grid,
                    previous_camera, tint);
                const auto current_only = Oracle::expectedFrame(
                    samples, grid, frame_camera, committed_phase, false, previous.m_rgb, previous_grid, previous_camera,
                    tint);
                const auto actual = fixture.readResolved(result.m_view, grid);
                u32 blended = 0u;
                for (i32 y = 0; y < grid.m_resolved.y; ++y) {
                    for (i32 x = 0; x < grid.m_resolved.x; ++x) {
                        const auto offset = static_cast<std::size_t>(y) * static_cast<std::size_t>(grid.m_backing.x) +
                                            static_cast<std::size_t>(x);
                        if (reset or Gpu::interiorBlendPixel(x, y, grid)) {
                            Gpu::assertClose(actual.m_rgb[offset], expected[offset], 3.0e-3f);
                        }
                        const float3 delta = expected[offset] - current_only[offset];
                        if (std::abs(delta.x) + std::abs(delta.y) + std::abs(delta.z) > 1.0e-4f) {
                            ++blended;
                        }
                    }
                }

                if (reset) {
                    PPR_TEST_ASSERT(blended == 0u);
                } else {
                    PPR_TEST_ASSERT(blended > 0u);
                }

                previous = actual;
                previous_camera = frame_camera;
                previous_grid = grid;
                previous_world = world;
                previous_cut = cut;
            };

            // World replacement resets; the next accepted frame re-establishes it.
            camera = makeOrthoCamera(window, float3{0.0f, 0.0f, -1.0f});
            check(*grid0, camera, world1, 0u, 0.3f);
            check(*grid0, camera, world1, 0u, 0.6f);

            // Explicit camera cut resets.
            check(*grid0, camera, world1, 1u, 0.8f);
            check(*grid0, camera, world1, 1u, 0.5f);

            // W/P/R/S/B change resets.
            camera = makeOrthoCamera(int2{23, 15}, float3{0.0f, 0.0f, -1.0f});
            check(*grid1, camera, world1, 1u, 0.4f);
            check(*grid1, camera, world1, 1u, 0.7f);

            // W-only change (same P/R/S/B) resets.
            const auto grid_w = makePresentationGrid(int2{22, 13}, 2u);
            PPR_TEST_ASSERT(grid_w.has_value());
            camera = makeOrthoCamera(int2{22, 13}, float3{0.0f, 0.0f, -1.0f});
            check(*grid_w, camera, world1, 1u, 0.2f);
            check(*grid_w, camera, world1, 1u, 0.3f);

            // Ordinary pan and zoom reproject; exposure/bloom/sky have no input
            // here, so an unchanged pass configuration also keeps history.
            camera = makeOrthoCamera(int2{22, 13}, float3{0.3f, 0.0f, -1.0f});
            check(*grid_w, camera, world1, 1u, 0.9f);
            camera = makeOrthoCamera(int2{22, 13}, float3{0.3f, 0.0f, -1.0f}, 1.5f);
            check(*grid_w, camera, world1, 1u, 0.6f);
            check(*grid_w, camera, world1, 1u, 0.5f);

            // P-only change (window unchanged) also resets.
            const auto grid_p = makePresentationGrid(int2{22, 13}, 1u);
            PPR_TEST_ASSERT(grid_p.has_value());
            camera = makeOrthoCamera(int2{22, 13}, float3{0.3f, 0.0f, -1.0f});
            check(*grid_p, camera, world1, 1u, 0.35f);
            check(*grid_p, camera, world1, 1u, 0.45f);
        };

        PPR_UNIT_TEST (gpu_uncommitted_attempt_does_not_advance_history) {
            Gpu::Fixture fixture;
            fixture.initialize();
            PPR_DEFER {
                fixture.shutdown();
            };

            const int2 window{17, 11};
            const auto grid = makePresentationGrid(window, 1u);
            PPR_TEST_ASSERT(grid.has_value());
            const auto world = Gpu::worldIdentity(9u);
            const auto samples = Oracle::makeProbeSamples(grid->m_raster, 1.0f);
            auto camera = makeOrthoCamera(window, float3{0.0f, 0.0f, -1.0f});
            auto other_camera = makeOrthoCamera(window, float3{1.3f, 0.4f, -1.0f});

            const auto first = fixture.runFrame(camera, *grid, world, 0u, 0u, float4{0.0f, 0.0f, 0.0f, 1.0f});
            const auto previous = fixture.readResolved(first.m_view, *grid);

            // beginFrame alone proposes phase 1 but commits nothing.
            PPR_TEST_ASSERT(not fixture.m_pass.beginFrame(
                fixture.m_rhi->getDevice(), other_camera, *grid, world, 0u, fixture.m_fence.completed()));
            const float2 proposed = fixture.m_pass.sceneCamera().m_jitter;
            fixture.m_pass.discardUnsubmitted();

            // A stage-A scene write whose stage B is abandoned also commits no
            // camera/history role.
            PPR_TEST_ASSERT(not fixture.m_pass.beginFrame(
                fixture.m_rhi->getDevice(), other_camera, *grid, world, 0u, fixture.m_fence.completed()));
            fixture.renderScene(*grid, 0u, float4{0.0f, 0.0f, 0.0f, 0.2f});
            fixture.m_pass.discardUnsubmitted();

            PPR_TEST_ASSERT(not fixture.m_pass.beginFrame(
                fixture.m_rhi->getDevice(), camera, *grid, world, 0u, fixture.m_fence.completed()));
            PPR_TEST_ASSERT(same(fixture.m_pass.sceneCamera().m_jitter, proposed));
            const auto result = fixture.finishFrame(*grid, 0u, float4{0.0f, 0.0f, 0.0f, 0.9f});

            const auto expected = Oracle::expectedFrame(
                samples, *grid, camera, 1u, true, previous.m_rgb, *grid, camera, 0.9f);
            const auto actual = fixture.readResolved(result.m_view, *grid);
            for (i32 y = 0; y < grid->m_resolved.y; ++y) {
                for (i32 x = 0; x < grid->m_resolved.x; ++x) {
                    const auto offset = static_cast<std::size_t>(y) * static_cast<std::size_t>(grid->m_backing.x) +
                                        static_cast<std::size_t>(x);
                    if (Gpu::interiorBlendPixel(x, y, *grid)) {
                        Gpu::assertClose(actual.m_rgb[offset], expected[offset], 3.0e-3f);
                    }
                }
            }
        };

        PPR_UNIT_TEST (gpu_unaccepted_extent_change_preserves_committed_history) {
            Gpu::Fixture fixture;
            fixture.initialize();
            PPR_DEFER {
                fixture.shutdown();
            };

            const int2 window0{17, 11};
            const int2 window1{19, 13};
            const auto grid0 = makePresentationGrid(window0, 1u);
            const auto grid1 = makePresentationGrid(window1, 1u);
            PPR_TEST_ASSERT(grid0.has_value() and grid1.has_value());
            const auto world = Gpu::worldIdentity(12u);
            const auto camera0 = makeOrthoCamera(window0, float3{0.0f, 0.0f, -1.0f});
            const auto camera1 = makeOrthoCamera(window1, float3{0.0f, 0.0f, -1.0f});
            const auto samples = Oracle::makeProbeSamples(grid0->m_raster, 1.0f);

            const auto first = fixture.runFrame(camera0, *grid0, world, 0u, 0u, float4{0.0f, 0.0f, 0.0f, 1.0f});
            auto committed_view = first.m_view;
            auto committed = fixture.readResolved(committed_view, *grid0);

            // Exposure/bloom/sky-hour edits never enter this pass. A frame with
            // the same camera/world/grid and a changed scene tint therefore
            // continues the accepted history instead of resetting it.
            const auto settings = fixture.runFrame(camera0, *grid0, world, 0u, 0u, float4{0.0f, 0.0f, 0.0f, 0.7f});
            const auto settings_expected = Oracle::expectedFrame(
                samples, *grid0, camera0, 1u, true, committed.m_rgb, *grid0, camera0, 0.7f);
            const auto settings_actual = fixture.readResolved(settings.m_view, *grid0);
            for (i32 y = 0; y < grid0->m_resolved.y; ++y) {
                for (i32 x = 0; x < grid0->m_resolved.x; ++x) {
                    const auto offset = static_cast<std::size_t>(y) * static_cast<std::size_t>(grid0->m_backing.x) +
                                        static_cast<std::size_t>(x);
                    if (Gpu::interiorBlendPixel(x, y, *grid0)) {
                        Gpu::assertClose(settings_actual.m_rgb[offset], settings_expected[offset], 3.0e-3f);
                    }
                }
            }
            committed_view = settings.m_view;
            committed = settings_actual;

            // A tentative G1 extent is allocated and its stage-A scene is even
            // submitted, but stage B is never accepted. The committed G0 pair
            // must remain byte-identical and usable.
            PPR_TEST_ASSERT(not fixture.m_pass.beginFrame(
                fixture.m_rhi->getDevice(), camera1, *grid1, world, 0u, fixture.m_fence.completed()));
            fixture.renderScene(*grid1, 0u, float4{0.0f, 0.0f, 0.0f, 0.2f});
            {
                auto &device = fixture.m_rhi->getDevice();
                rhi::ComPtr<rhi::ICommandEncoder> encoder;
                PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(encoder.writeRef())));
                const PreRenderContext context{device, *encoder, fixture.m_fence.completed(), {}};
                PPR_TEST_ASSERT(not fixture.m_pass.prepare(context));
            }
            fixture.m_pass.discardUnsubmitted();

            const auto after_discard = fixture.readResolved(committed_view, *grid0);
            PPR_TEST_ASSERT(after_discard.m_rgb.size() == committed.m_rgb.size());
            for (std::size_t index = 0u; index < committed.m_rgb.size(); ++index) {
                PPR_TEST_ASSERT(after_discard.m_rgb[index].x == committed.m_rgb[index].x);
                PPR_TEST_ASSERT(after_discard.m_rgb[index].y == committed.m_rgb[index].y);
                PPR_TEST_ASSERT(after_discard.m_rgb[index].z == committed.m_rgb[index].z);
            }

            // A same-extent reset request (W-only change, identical S/B) reuses
            // the committed pair but writes its non-committed member, so even
            // that definite discard leaves the accepted history bytes intact.
            const auto grid_w = makePresentationGrid(int2{18, 11}, 1u);
            PPR_TEST_ASSERT(grid_w.has_value());
            const auto camera_w = makeOrthoCamera(int2{18, 11}, float3{0.0f, 0.0f, -1.0f});
            PPR_TEST_ASSERT(not fixture.m_pass.beginFrame(
                fixture.m_rhi->getDevice(), camera_w, *grid_w, world, 0u, fixture.m_fence.completed()));
            fixture.m_pass.discardUnsubmitted();
            const auto after_extent_match_discard = fixture.readResolved(committed_view, *grid0);
            for (std::size_t index = 0u; index < committed.m_rgb.size(); ++index) {
                PPR_TEST_ASSERT(after_extent_match_discard.m_rgb[index].x == committed.m_rgb[index].x);
                PPR_TEST_ASSERT(after_extent_match_discard.m_rgb[index].y == committed.m_rgb[index].y);
                PPR_TEST_ASSERT(after_extent_match_discard.m_rgb[index].z == committed.m_rgb[index].z);
            }

            // The next accepted G0 frame must advance phase exactly once and
            // blend against the old G0 history, not uninitialized bytes.
            PPR_TEST_ASSERT(not fixture.m_pass.beginFrame(
                fixture.m_rhi->getDevice(), camera0, *grid0, world, 0u, fixture.m_fence.completed()));
            const float2 jitter = Gpu::phaseJitter(2u);
            PPR_TEST_ASSERT(same(
                fixture.m_pass.sceneCamera().m_jitter, float2{
                                                           2.0f * jitter.x / static_cast<float>(grid0->m_raster.x),
                                                           -2.0f * jitter.y / static_cast<float>(grid0->m_raster.y)}));
            const auto result = fixture.finishFrame(*grid0, 0u, float4{0.0f, 0.0f, 0.0f, 0.6f});
            const auto expected = Oracle::expectedFrame(
                samples, *grid0, camera0, 2u, true, committed.m_rgb, *grid0, camera0, 0.6f);
            const auto actual = fixture.readResolved(result.m_view, *grid0);
            for (i32 y = 0; y < grid0->m_resolved.y; ++y) {
                for (i32 x = 0; x < grid0->m_resolved.x; ++x) {
                    const auto offset = static_cast<std::size_t>(y) * static_cast<std::size_t>(grid0->m_backing.x) +
                                        static_cast<std::size_t>(x);
                    if (Gpu::interiorBlendPixel(x, y, *grid0)) {
                        Gpu::assertClose(actual.m_rgb[offset], expected[offset], 3.0e-3f);
                    }
                }
            }
        };

        PPR_UNIT_TEST (gpu_busy_extent_change_preserves_committed_history) {
            Gpu::Fixture fixture;
            fixture.initialize();
            PPR_DEFER {
                fixture.shutdown();
            };

            const int2 window0{13, 9};
            const int2 window1{15, 11};
            const auto grid0 = makePresentationGrid(window0, 1u);
            const auto grid1 = makePresentationGrid(window1, 1u);
            PPR_TEST_ASSERT(grid0.has_value() and grid1.has_value());
            const auto world = Gpu::worldIdentity(13u);
            const auto camera0 = makeOrthoCamera(window0, float3{0.0f, 0.0f, -1.0f});
            const auto camera1 = makeOrthoCamera(window1, float3{0.0f, 0.0f, -1.0f});
            const auto samples = Oracle::makeProbeSamples(grid0->m_raster, 1.0f);

            // Three submitted-but-uncompleted frames occupy every root slot.
            Gpu::Fixture::FrameResult third{};
            for (u32 index = 0u; index < 3u; ++index) {
                PPR_TEST_ASSERT(not fixture.m_pass.beginFrame(
                    fixture.m_rhi->getDevice(), camera0, *grid0, world, 0u, fixture.m_fence.completed()));
                third = fixture.finishFrame(*grid0, 0u, float4{0.0f, 0.0f, 0.0f, 1.0f}, false);
            }

            // Busy admission proposes nothing and allocates nothing.
            PPR_TEST_ASSERT(
                fixture.m_pass.beginFrame(
                    fixture.m_rhi->getDevice(), camera1, *grid1, world, 0u, fixture.m_fence.completed()) ==
                std::errc::device_or_resource_busy);

            PPR_TEST_ASSERT(not fixture.m_fence.waitFor(fixture.m_last_value));
            PPR_TEST_ASSERT(not fixture.m_pass.collectCompleted(fixture.m_fence.completed()));
            const auto reference = fixture.readResolved(third.m_view, *grid0);

            PPR_TEST_ASSERT(not fixture.m_pass.beginFrame(
                fixture.m_rhi->getDevice(), camera0, *grid0, world, 0u, fixture.m_fence.completed()));
            const auto result = fixture.finishFrame(*grid0, 0u, float4{0.0f, 0.0f, 0.0f, 0.7f});
            const auto expected = Oracle::expectedFrame(
                samples, *grid0, camera0, 3u, true, reference.m_rgb, *grid0, camera0, 0.7f);
            const auto actual = fixture.readResolved(result.m_view, *grid0);
            for (i32 y = 0; y < grid0->m_resolved.y; ++y) {
                for (i32 x = 0; x < grid0->m_resolved.x; ++x) {
                    const auto offset = static_cast<std::size_t>(y) * static_cast<std::size_t>(grid0->m_backing.x) +
                                        static_cast<std::size_t>(x);
                    if (Gpu::interiorBlendPixel(x, y, *grid0)) {
                        Gpu::assertClose(actual.m_rgb[offset], expected[offset], 3.0e-3f);
                    }
                }
            }
        };

        PPR_UNIT_TEST (gpu_resource_reuse_waits_for_reader_fence) {
            Gpu::Fixture fixture;
            fixture.initialize();
            PPR_DEFER {
                fixture.shutdown();
            };

            const int2 window{13, 9};
            const auto grid = makePresentationGrid(window, 1u);
            PPR_TEST_ASSERT(grid.has_value());
            const auto world = Gpu::worldIdentity(10u);
            const auto camera = makeOrthoCamera(window, float3{0.0f, 0.0f, -1.0f});
            auto &device = fixture.m_rhi->getDevice();

            // Three submitted-but-uncompleted frames occupy all root slots.
            for (u32 index = 0u; index < 3u; ++index) {
                PPR_TEST_ASSERT(
                    not fixture.m_pass.beginFrame(device, camera, *grid, world, 0u, fixture.m_fence.completed()));
                const auto result = fixture.finishFrame(*grid, 0u, float4{0.0f, 0.0f, 0.0f, 1.0f}, false);
                std::ignore = result;
            }

            PPR_TEST_ASSERT(
                fixture.m_pass.beginFrame(device, camera, *grid, world, 0u, fixture.m_fence.completed()) ==
                std::errc::device_or_resource_busy);

            // A different extent allocates new resources while the old ones stay
            // deferred for their readers; it does not overwrite them in flight.
            const auto resized = makePresentationGrid(int2{15, 9}, 1u);
            PPR_TEST_ASSERT(resized.has_value());
            PPR_TEST_ASSERT(
                fixture.m_pass.beginFrame(device, camera, *resized, world, 0u, fixture.m_fence.completed()) ==
                std::errc::device_or_resource_busy);

            PPR_TEST_ASSERT(not fixture.m_fence.waitFor(fixture.m_last_value));
            PPR_TEST_ASSERT(not fixture.m_pass.collectCompleted(fixture.m_fence.completed()));
            PPR_TEST_ASSERT(
                not fixture.m_pass.beginFrame(device, camera, *resized, world, 0u, fixture.m_fence.completed()));
            fixture.m_pass.discardUnsubmitted();
        };

        // ------------------------------------------------------------------
        // reconstruction groups
        // ------------------------------------------------------------------

        const UnitTest cpu = UnitTest::Named("cpu") / [](UnitTest::IRun &run) -> void {
            run.recurse({
                presentation_extents_and_caller_suspension,
                two_resolution_projection_alignment,
                lattice_known_nodes_and_signed_floor_division,
                lattice_geometric_scan_brackets_unknown_nodes,
                lattice_diamond_interpolation_and_original_sample_bounds,
                lattice_boundary_replication_and_tie_order,
                lattice_stationary_positions_do_not_drift,
                motion_weight_curve,
                history_footprint_and_poison_padding_excluded,
                history_requires_every_nonzero_weight_texel_inside_r,
            });
        };

        const UnitTest gpu = UnitTest::Named("gpu") / [](UnitTest::IRun &run) -> void {
            run.recurse({
                gpu_exact_extents_projection_and_caller_suspension,
                gpu_probe_samples_match_cpu_pattern,
                gpu_lattice_matches_independent_oracle_all_phases,
                gpu_constant_color_every_phase,
                gpu_temporal_motion_and_offscreen_history,
                gpu_padding_history_is_never_read,
                gpu_history_reset_and_preserve_matrix,
                gpu_uncommitted_attempt_does_not_advance_history,
                gpu_unaccepted_extent_change_preserves_committed_history,
                gpu_busy_extent_change_preserves_committed_history,
                gpu_resource_reuse_waits_for_reader_fence,
            });
        };
    } // namespace Reconstruction
} // namespace pP::tests::detail

namespace pP::tests {
    // ------------------------------------------------------------------
    // reconstruction suite
    // ------------------------------------------------------------------

    const UnitTest reconstruction_contracts = UnitTest::Named("reconstruction") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::Reconstruction::cpu,
            detail::Reconstruction::gpu,
        });
    };

    const UnitTest &reconstructionTests() noexcept {
        return reconstruction_contracts;
    }
} // namespace pP::tests
