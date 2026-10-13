module;
#include "pP/UnitTest.h"

module engine.tests.core;

import engine.core;
import engine.math;
import std;

namespace pP::tests::detail {
    // ------------------------------------------------------------------
    // math
    // ------------------------------------------------------------------

    namespace Math {
        double contourCross(const math::ContourPoint a, const math::ContourPoint b, const math::ContourPoint p) {
            return (b.m_x - a.m_x) * (p.m_y - a.m_y) - (b.m_y - a.m_y) * (p.m_x - a.m_x);
        }

        bool contourContains(const math::ContourCase &shape, const math::ContourPoint point) {
            const auto vertices = math::contourPoints();
            for (u32 i = 0u; i < shape.m_triangle_count; ++i) {
                const auto triangle = shape.m_triangles[i];
                if (contourCross(vertices[triangle.m_a], vertices[triangle.m_b], point) >= 0.0 and
                    contourCross(vertices[triangle.m_b], vertices[triangle.m_c], point) >= 0.0 and
                    contourCross(vertices[triangle.m_c], vertices[triangle.m_a], point) >= 0.0) {
                    return true;
                }
            }
            return false;
        }

        PPR_UNIT_TEST (contour_all_cases_have_exact_area_and_oriented_boundaries) {
            constexpr std::array<double, 16> expected_areas{
                0.0, 0.125, 0.125, 0.5, 0.125, 0.25, 0.5, 0.875, 0.125, 0.5, 0.25, 0.875, 0.5, 0.875, 0.875, 1.0};
            const auto vertices = math::contourPoints();
            const auto cases = math::contourCases();
            static_assert(std::is_trivially_copyable_v<math::ContourCase>);
            static_assert(std::is_standard_layout_v<math::ContourCase>);
            static_assert(sizeof(math::ContourPoint) == 8u);
            static_assert(sizeof(math::ContourSegment) == 8u);
            static_assert(sizeof(math::ContourTriangle) == 16u);
            static_assert(sizeof(math::ContourCase) == 72u);

            for (u32 bits = 0u; bits < 16u; ++bits) {
                const auto &shape = cases[bits];
                PPR_TEST_ASSERT(shape.m_segment_count <= 2u and shape.m_triangle_count <= 3u);
                double area = 0.0;
                for (u32 i = 0u; i < shape.m_triangle_count; ++i) {
                    const auto triangle = shape.m_triangles[i];
                    PPR_TEST_ASSERT(triangle.m_a < 8u and triangle.m_b < 8u and triangle.m_c < 8u);
                    PPR_TEST_ASSERT(triangle.m_solid_corners != 0u and (triangle.m_solid_corners & ~bits) == 0u);
                    const double twice_area = contourCross(
                        vertices[triangle.m_a], vertices[triangle.m_b], vertices[triangle.m_c]);
                    PPR_TEST_ASSERT(twice_area > 0.0);
                    area += twice_area * 0.5;
                }
                PPR_TEST_ASSERT(area == expected_areas[bits]);
                for (u32 corner = 0u; corner < 4u; ++corner) {
                    PPR_TEST_ASSERT(contourContains(shape, vertices[corner]) == ((bits & (1u << corner)) != 0u));
                }
                for (u32 i = 0u; i < shape.m_segment_count; ++i) {
                    const auto segment = shape.m_segments[i];
                    PPR_TEST_ASSERT(segment.m_begin >= 4u and segment.m_begin < 8u);
                    PPR_TEST_ASSERT(segment.m_end >= 4u and segment.m_end < 8u);
                    const auto a = vertices[segment.m_begin];
                    const auto b = vertices[segment.m_end];
                    const float mid_x = (a.m_x + b.m_x) * 0.5f;
                    const float mid_y = (a.m_y + b.m_y) * 0.5f;
                    constexpr float step = 1.0f / 1024.0f;
                    const math::ContourPoint left{mid_x - (b.m_y - a.m_y) * step, mid_y + (b.m_x - a.m_x) * step};
                    const math::ContourPoint right{mid_x + (b.m_y - a.m_y) * step, mid_y - (b.m_x - a.m_x) * step};
                    PPR_TEST_ASSERT(contourContains(shape, left));
                    PPR_TEST_ASSERT(not contourContains(shape, right));
                }
            }
        };

        PPR_UNIT_TEST (contour_saddles_are_disconnected_and_neighbour_edges_agree) {
            const auto cases = math::contourCases();
            for (const u32 bits: {5u, 10u}) {
                const auto &shape = cases[bits];
                PPR_TEST_ASSERT(shape.m_segment_count == 2u and shape.m_triangle_count == 2u);
                PPR_TEST_ASSERT(not contourContains(shape, {0.5f, 0.5f}));
                const auto a = shape.m_triangles[0u].m_solid_corners;
                const auto b = shape.m_triangles[1u].m_solid_corners;
                PPR_TEST_ASSERT(std::has_single_bit(a) and std::has_single_bit(b));
                PPR_TEST_ASSERT((a | b) == bits and (a & b) == 0u);
            }
            for (u32 a = 0u; a < 16u; ++a) {
                for (u32 b = 0u; b < 16u; ++b) {
                    const bool right_matches_left = ((a >> 1u) & 1u) == (b & 1u) and
                                                    ((a >> 2u) & 1u) == ((b >> 3u) & 1u);
                    const bool top_matches_bottom = ((a >> 3u) & 1u) == (b & 1u) and
                                                    ((a >> 2u) & 1u) == ((b >> 1u) & 1u);
                    for (const float t: {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
                        if (right_matches_left) {
                            PPR_TEST_ASSERT(
                                contourContains(cases[a], {1.0f, t}) == contourContains(cases[b], {0.0f, t}));
                        }
                        if (top_matches_bottom) {
                            PPR_TEST_ASSERT(
                                contourContains(cases[a], {t, 1.0f}) == contourContains(cases[b], {t, 0.0f}));
                        }
                    }
                }
            }
        };

        PPR_UNIT_TEST (scalar_rounding_to_int) {
            static_assert(std::same_as<decltype(ceilToInt(1.0f)), i32>);
            static_assert(std::same_as<decltype(floorToInt(1.0f)), i32>);
            static_assert(std::same_as<decltype(roundToInt(1.0f)), i32>);
            static_assert(std::same_as<decltype(truncToInt(1.0f)), i32>);

            PPR_TEST_ASSERT(ceilToInt(-1.25f) == -1);
            PPR_TEST_ASSERT(floorToInt(-1.25f) == -2);
            PPR_TEST_ASSERT(roundToInt(-1.5f) == -2);
            PPR_TEST_ASSERT(roundToInt(2.5f) == 3);
            PPR_TEST_ASSERT(roundToInt(-2.5f) == -3);
            PPR_TEST_ASSERT(truncToInt(-1.75f) == -1);
        };

        PPR_UNIT_TEST (scalar_rounding_to_uint) {
            static_assert(std::same_as<decltype(ceilToUInt(1.0f)), u32>);
            static_assert(std::same_as<decltype(floorToUInt(1.0f)), u32>);
            static_assert(std::same_as<decltype(roundToUInt(1.0f)), u32>);
            static_assert(std::same_as<decltype(truncToUInt(1.0f)), u32>);

            PPR_TEST_ASSERT(ceilToUInt(1.25f) == 2u);
            PPR_TEST_ASSERT(floorToUInt(1.25f) == 1u);
            PPR_TEST_ASSERT(roundToUInt(1.5f) == 2u);
            PPR_TEST_ASSERT(roundToUInt(2.5f) == 3u);
            PPR_TEST_ASSERT(truncToUInt(1.75f) == 1u);
        };

        PPR_UNIT_TEST (float2_rounding_to_int) {
            const float2 value{-1.25f, 1.75f};

            const int2 ceil_value = ceilToInt<float, 2u>(value);
            const int2 floor_value = floorToInt<float, 2u>(value);
            const int2 round_value = roundToInt<float, 2u>(value);
            const int2 trunc_value = truncToInt<float, 2u>(value);

            PPR_TEST_ASSERT(ceil_value.x == -1 and ceil_value.y == 2);
            PPR_TEST_ASSERT(floor_value.x == -2 and floor_value.y == 1);
            PPR_TEST_ASSERT(round_value.x == -1 and round_value.y == 2);
            PPR_TEST_ASSERT(trunc_value.x == -1 and trunc_value.y == 1);
        };

        PPR_UNIT_TEST (float3_rounding_to_uint) {
            const float3 value{1.25f, 2.75f, 3.5f};

            const uint3 ceil_value = ceilToUInt<float, 3u>(value);
            const uint3 floor_value = floorToUInt<float, 3u>(value);
            const uint3 round_value = roundToUInt<float, 3u>(value);
            const uint3 trunc_value = truncToUInt<float, 3u>(value);

            PPR_TEST_ASSERT(ceil_value.x == 2u and ceil_value.y == 3u and ceil_value.z == 4u);
            PPR_TEST_ASSERT(floor_value.x == 1u and floor_value.y == 2u and floor_value.z == 3u);
            PPR_TEST_ASSERT(round_value.x == 1u and round_value.y == 3u and round_value.z == 4u);
            PPR_TEST_ASSERT(trunc_value.x == 1u and trunc_value.y == 2u and trunc_value.z == 3u);
        };

        PPR_UNIT_TEST (float4_rounding) {
            const float4 signed_value{-1.25f, -2.75f, 3.5f, 4.25f};
            const float4 unsigned_value{1.25f, 2.75f, 3.5f, 4.25f};
            const float4 signed_tie_value{2.5f, -2.5f, 0.0f, 0.0f};
            const float4 unsigned_tie_value{2.5f, 0.0f, 0.0f, 0.0f};

            const int4 signed_ceil = ceilToInt<float, 4u>(signed_value);
            const int4 signed_floor = floorToInt<float, 4u>(signed_value);
            const int4 signed_round = roundToInt<float, 4u>(signed_value);
            const int4 signed_trunc = truncToInt<float, 4u>(signed_value);
            const uint4 unsigned_ceil = ceilToUInt<float, 4u>(unsigned_value);
            const uint4 unsigned_floor = floorToUInt<float, 4u>(unsigned_value);
            const uint4 unsigned_round = roundToUInt<float, 4u>(unsigned_value);
            const uint4 unsigned_trunc = truncToUInt<float, 4u>(unsigned_value);
            const int4 signed_tie_round = roundToInt<float, 4u>(signed_tie_value);
            const uint4 unsigned_tie_round = roundToUInt<float, 4u>(unsigned_tie_value);

            PPR_TEST_ASSERT(signed_ceil.x == -1 and signed_ceil.y == -2 and signed_ceil.z == 4 and signed_ceil.w == 5);
            PPR_TEST_ASSERT(
                signed_floor.x == -2 and signed_floor.y == -3 and signed_floor.z == 3 and signed_floor.w == 4);
            PPR_TEST_ASSERT(
                signed_round.x == -1 and signed_round.y == -3 and signed_round.z == 4 and signed_round.w == 4);
            PPR_TEST_ASSERT(
                signed_trunc.x == -1 and signed_trunc.y == -2 and signed_trunc.z == 3 and signed_trunc.w == 4);
            PPR_TEST_ASSERT(
                unsigned_ceil.x == 2u and unsigned_ceil.y == 3u and unsigned_ceil.z == 4u and unsigned_ceil.w == 5u);
            PPR_TEST_ASSERT(
                unsigned_floor.x == 1u and
                unsigned_floor.y == 2u and
                unsigned_floor.z == 3u and
                unsigned_floor.w == 4u);
            PPR_TEST_ASSERT(
                unsigned_round.x == 1u and
                unsigned_round.y == 3u and
                unsigned_round.z == 4u and
                unsigned_round.w == 4u);
            PPR_TEST_ASSERT(
                unsigned_trunc.x == 1u and
                unsigned_trunc.y == 2u and
                unsigned_trunc.z == 3u and
                unsigned_trunc.w == 4u);
            PPR_TEST_ASSERT(signed_tie_round.x == 3 and signed_tie_round.y == -3);
            PPR_TEST_ASSERT(unsigned_tie_round.x == 3u);
        };

        PPR_UNIT_TEST (double_rounding_fallbacks) {
            const double2 signed_value{-1.25, 2.75};
            const double4 unsigned_value{1.25, 2.75, 3.5, 4.25};

            const int2 signed_ceil = ceilToInt<double, 2u>(signed_value);
            const int2 signed_floor = floorToInt<double, 2u>(signed_value);
            const int2 signed_round = roundToInt<double, 2u>(signed_value);
            const int2 signed_trunc = truncToInt<double, 2u>(signed_value);
            const uint4 unsigned_ceil = ceilToUInt<double, 4u>(unsigned_value);
            const uint4 unsigned_floor = floorToUInt<double, 4u>(unsigned_value);
            const uint4 unsigned_round = roundToUInt<double, 4u>(unsigned_value);
            const uint4 unsigned_trunc = truncToUInt<double, 4u>(unsigned_value);

            PPR_TEST_ASSERT(signed_ceil.x == -1 and signed_ceil.y == 3);
            PPR_TEST_ASSERT(signed_floor.x == -2 and signed_floor.y == 2);
            PPR_TEST_ASSERT(signed_round.x == -1 and signed_round.y == 3);
            PPR_TEST_ASSERT(signed_trunc.x == -1 and signed_trunc.y == 2);
            PPR_TEST_ASSERT(
                unsigned_ceil.x == 2u and unsigned_ceil.y == 3u and unsigned_ceil.z == 4u and unsigned_ceil.w == 5u);
            PPR_TEST_ASSERT(
                unsigned_floor.x == 1u and
                unsigned_floor.y == 2u and
                unsigned_floor.z == 3u and
                unsigned_floor.w == 4u);
            PPR_TEST_ASSERT(
                unsigned_round.x == 1u and
                unsigned_round.y == 3u and
                unsigned_round.z == 4u and
                unsigned_round.w == 4u);
            PPR_TEST_ASSERT(
                unsigned_trunc.x == 1u and
                unsigned_trunc.y == 2u and
                unsigned_trunc.z == 3u and
                unsigned_trunc.w == 4u);
        };

        PPR_UNIT_TEST (to_float_from_integral_vectors) {
            const int4 signed_value{-1, 2, -3, 4};
            const uint4 unsigned_value{1u, 2u, 3u, 4u};
            math::Vector<i32, 5> fallback_value{};
            fallback_value[0] = -1;
            fallback_value[1] = 2;
            fallback_value[2] = -3;
            fallback_value[3] = 4;
            fallback_value[4] = -5;

            const float4 signed_float = toFloat<int, 4u>(signed_value);
            const float4 unsigned_float = toFloat<u32, 4u>(unsigned_value);
            const math::Vector<float, 5> fallback_float = toFloat<i32, 5u>(fallback_value);

            PPR_TEST_ASSERT(
                signed_float.x == -1.0f and
                signed_float.y == 2.0f and
                signed_float.z == -3.0f and
                signed_float.w == 4.0f);
            PPR_TEST_ASSERT(
                unsigned_float.x == 1.0f and
                unsigned_float.y == 2.0f and
                unsigned_float.z == 3.0f and
                unsigned_float.w == 4.0f);
            PPR_TEST_ASSERT(
                fallback_float[0] == -1.0f and
                fallback_float[1] == 2.0f and
                fallback_float[2] == -3.0f and
                fallback_float[3] == 4.0f and
                fallback_float[4] == -5.0f);
        };

        PPR_UNIT_TEST (generic_vector_rounding_fallback) {
            math::Vector<float, 5> value{};
            value[0] = -1.25f;
            value[1] = -2.75f;
            value[2] = 3.5f;
            value[3] = 4.25f;
            value[4] = 5.75f;
            const math::Vector<i32, 5> result = truncToInt<float, 5u>(value);

            PPR_TEST_ASSERT(
                result[0] == -1 and result[1] == -2 and result[2] == 3 and result[3] == 4 and result[4] == 5);
        };
    }
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest math = UnitTest::Named("math") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::Math::scalar_rounding_to_int,
            detail::Math::scalar_rounding_to_uint,
            detail::Math::float2_rounding_to_int,
            detail::Math::float3_rounding_to_uint,
            detail::Math::float4_rounding,
            detail::Math::double_rounding_fallbacks,
            detail::Math::to_float_from_integral_vectors,
            detail::Math::generic_vector_rounding_fallback,
            detail::Math::contour_all_cases_have_exact_area_and_oriented_boundaries,
            detail::Math::contour_saddles_are_disconnected_and_neighbour_edges_agree,
        });
    };

    const UnitTest &mathTests() noexcept {
        return math;
    }
} // namespace pP::tests
