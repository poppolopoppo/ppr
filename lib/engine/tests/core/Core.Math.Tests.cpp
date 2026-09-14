module;
#include "pP/UnitTest.h"

module engine.tests.core;

import engine.core;
import engine.math;
import std;

namespace pP::tests::detail {
    namespace Math {
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

            const int2 ceil_value = ceilToInt(value);
            const int2 floor_value = floorToInt(value);
            const int2 round_value = roundToInt(value);
            const int2 trunc_value = truncToInt(value);

            PPR_TEST_ASSERT(ceil_value.x == -1 and ceil_value.y == 2);
            PPR_TEST_ASSERT(floor_value.x == -2 and floor_value.y == 1);
            PPR_TEST_ASSERT(round_value.x == -1 and round_value.y == 2);
            PPR_TEST_ASSERT(trunc_value.x == -1 and trunc_value.y == 1);
        };

        PPR_UNIT_TEST (float3_rounding_to_uint) {
            const float3 value{1.25f, 2.75f, 3.5f};

            const uint3 ceil_value = ceilToUInt(value);
            const uint3 floor_value = floorToUInt(value);
            const uint3 round_value = roundToUInt(value);
            const uint3 trunc_value = truncToUInt(value);

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

            const int4 signed_ceil = ceilToInt(signed_value);
            const int4 signed_floor = floorToInt(signed_value);
            const int4 signed_round = roundToInt(signed_value);
            const int4 signed_trunc = truncToInt(signed_value);
            const uint4 unsigned_ceil = ceilToUInt(unsigned_value);
            const uint4 unsigned_floor = floorToUInt(unsigned_value);
            const uint4 unsigned_round = roundToUInt(unsigned_value);
            const uint4 unsigned_trunc = truncToUInt(unsigned_value);
            const int4 signed_tie_round = roundToInt(signed_tie_value);
            const uint4 unsigned_tie_round = roundToUInt(unsigned_tie_value);

            PPR_TEST_ASSERT(signed_ceil.x == -1 and signed_ceil.y == -2 and signed_ceil.z == 4 and signed_ceil.w == 5);
            PPR_TEST_ASSERT(signed_floor.x == -2 and signed_floor.y == -3 and signed_floor.z == 3 and signed_floor.w == 4);
            PPR_TEST_ASSERT(signed_round.x == -1 and signed_round.y == -3 and signed_round.z == 4 and signed_round.w == 4);
            PPR_TEST_ASSERT(signed_trunc.x == -1 and signed_trunc.y == -2 and signed_trunc.z == 3 and signed_trunc.w == 4);
            PPR_TEST_ASSERT(unsigned_ceil.x == 2u and unsigned_ceil.y == 3u and unsigned_ceil.z == 4u and unsigned_ceil.w == 5u);
            PPR_TEST_ASSERT(unsigned_floor.x == 1u and unsigned_floor.y == 2u and unsigned_floor.z == 3u and unsigned_floor.w == 4u);
            PPR_TEST_ASSERT(unsigned_round.x == 1u and unsigned_round.y == 3u and unsigned_round.z == 4u and unsigned_round.w == 4u);
            PPR_TEST_ASSERT(unsigned_trunc.x == 1u and unsigned_trunc.y == 2u and unsigned_trunc.z == 3u and unsigned_trunc.w == 4u);
            PPR_TEST_ASSERT(signed_tie_round.x == 3 and signed_tie_round.y == -3);
            PPR_TEST_ASSERT(unsigned_tie_round.x == 3u);
        };

        PPR_UNIT_TEST (double_rounding_fallbacks) {
            const double2 signed_value{-1.25, 2.75};
            const double4 unsigned_value{1.25, 2.75, 3.5, 4.25};

            const int2 signed_ceil = ceilToInt(signed_value);
            const int2 signed_floor = floorToInt(signed_value);
            const int2 signed_round = roundToInt(signed_value);
            const int2 signed_trunc = truncToInt(signed_value);
            const uint4 unsigned_ceil = ceilToUInt(unsigned_value);
            const uint4 unsigned_floor = floorToUInt(unsigned_value);
            const uint4 unsigned_round = roundToUInt(unsigned_value);
            const uint4 unsigned_trunc = truncToUInt(unsigned_value);

            PPR_TEST_ASSERT(signed_ceil.x == -1 and signed_ceil.y == 3);
            PPR_TEST_ASSERT(signed_floor.x == -2 and signed_floor.y == 2);
            PPR_TEST_ASSERT(signed_round.x == -1 and signed_round.y == 3);
            PPR_TEST_ASSERT(signed_trunc.x == -1 and signed_trunc.y == 2);
            PPR_TEST_ASSERT(unsigned_ceil.x == 2u and unsigned_ceil.y == 3u and unsigned_ceil.z == 4u and unsigned_ceil.w == 5u);
            PPR_TEST_ASSERT(unsigned_floor.x == 1u and unsigned_floor.y == 2u and unsigned_floor.z == 3u and unsigned_floor.w == 4u);
            PPR_TEST_ASSERT(unsigned_round.x == 1u and unsigned_round.y == 3u and unsigned_round.z == 4u and unsigned_round.w == 4u);
            PPR_TEST_ASSERT(unsigned_trunc.x == 1u and unsigned_trunc.y == 2u and unsigned_trunc.z == 3u and unsigned_trunc.w == 4u);
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

            const float4 signed_float = toFloat(signed_value);
            const float4 unsigned_float = toFloat(unsigned_value);
            const math::Vector<float, 5> fallback_float = toFloat(fallback_value);

            PPR_TEST_ASSERT(signed_float.x == -1.0f and signed_float.y == 2.0f and signed_float.z == -3.0f and signed_float.w == 4.0f);
            PPR_TEST_ASSERT(unsigned_float.x == 1.0f and unsigned_float.y == 2.0f and unsigned_float.z == 3.0f and unsigned_float.w == 4.0f);
            PPR_TEST_ASSERT(
                fallback_float[0] == -1.0f and fallback_float[1] == 2.0f and fallback_float[2] == -3.0f and fallback_float[3] == 4.0f and fallback_float[4] == -5.0f);
        };

        PPR_UNIT_TEST (generic_vector_rounding_fallback) {
            math::Vector<float, 5> value{};
            value[0] = -1.25f;
            value[1] = -2.75f;
            value[2] = 3.5f;
            value[3] = 4.25f;
            value[4] = 5.75f;
            const math::Vector<i32, 5> result = truncToInt(value);

            PPR_TEST_ASSERT(result[0] == -1 and result[1] == -2 and result[2] == 3 and result[3] == 4 and result[4] == 5);
        };
    }
} // namespace pP::tests::detail

namespace pP::tests {
    extern const UnitTest math = UnitTest::Named("math") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::Math::scalar_rounding_to_int,
            detail::Math::scalar_rounding_to_uint,
            detail::Math::float2_rounding_to_int,
            detail::Math::float3_rounding_to_uint,
            detail::Math::float4_rounding,
            detail::Math::double_rounding_fallbacks,
            detail::Math::to_float_from_integral_vectors,
            detail::Math::generic_vector_rounding_fallback,
        });
    };
} // namespace pP::tests
