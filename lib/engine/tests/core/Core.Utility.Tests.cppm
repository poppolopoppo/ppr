module;
#include "pP/UnitTest.h"
export module engine.tests.core:utility;
import engine.core;
import std;

export namespace pP::tests {
    namespace Utility {
        PPR_UNIT_TEST(divide_round_up_basic) {
            constexpr auto exact = divideRoundUp(9u, 3u);
            PPR_TEST_ASSERT(exact == 3u);

            constexpr auto remainder = divideRoundUp(10u, 3u);
            PPR_TEST_ASSERT(remainder == 4u);

            constexpr auto zero_value = divideRoundUp(0u, 5u);
            PPR_TEST_ASSERT(zero_value == 0u);

            constexpr auto single_unit = divideRoundUp(1u, 5u);
            PPR_TEST_ASSERT(single_unit == 1u);

            constexpr auto equal = divideRoundUp(5u, 5u);
            PPR_TEST_ASSERT(equal == 1u);

            constexpr auto large = divideRoundUp(1000u, 3u);
            PPR_TEST_ASSERT(large == 334u);
        };

        PPR_UNIT_TEST(align_backward_basic) {
            constexpr auto aligned = alignBackward(8u, 8u);
            PPR_TEST_ASSERT(aligned == 8u);

            constexpr auto needs_rounding = alignBackward(10u, 8u);
            PPR_TEST_ASSERT(needs_rounding == 8u);

            constexpr auto zero = alignBackward(0u, 8u);
            PPR_TEST_ASSERT(zero == 0u);

            constexpr auto below_alignment = alignBackward(7u, 8u);
            PPR_TEST_ASSERT(below_alignment == 0u);

            constexpr auto multiple = alignBackward(24u, 8u);
            PPR_TEST_ASSERT(multiple == 24u);

            constexpr auto power_of_2 = alignBackward(15u, 16u);
            PPR_TEST_ASSERT(power_of_2 == 0u);
        };

        PPR_UNIT_TEST(align_forward_basic) {
            constexpr auto aligned = alignForward(8u, 8u);
            PPR_TEST_ASSERT(aligned == 8u);

            constexpr auto needs_rounding = alignForward(10u, 8u);
            PPR_TEST_ASSERT(needs_rounding == 16u);

            constexpr auto zero = alignForward(0u, 8u);
            PPR_TEST_ASSERT(zero == 0u);

            constexpr auto one = alignForward(1u, 8u);
            PPR_TEST_ASSERT(one == 8u);

            constexpr auto exact = alignForward(16u, 8u);
            PPR_TEST_ASSERT(exact == 16u);

            constexpr auto single = alignForward(1u, 1u);
            PPR_TEST_ASSERT(single == 1u);
        };

        PPR_UNIT_TEST(align_forward_align_val) {
            constexpr auto a1 = alignForward(10u, std::align_val_t{8});
            PPR_TEST_ASSERT(a1 == 16u);

            constexpr auto a2 = alignForward(8u, std::align_val_t{8});
            PPR_TEST_ASSERT(a2 == 8u);

            constexpr auto a3 = alignForward(0u, std::align_val_t{8});
            PPR_TEST_ASSERT(a3 == 0u);

            constexpr auto a4 = alignForward(1u, std::align_val_t{16});
            PPR_TEST_ASSERT(a4 == 16u);
        };

        PPR_UNIT_TEST(align_pointer_forward) {
            int data[16]{};
            const auto base = reinterpret_cast<std::uintptr_t>(data);
            auto *unaligned = reinterpret_cast<int *>(base + 1);
            auto *aligned = alignForward(unaligned, std::align_val_t{4});
            PPR_TEST_ASSERT(reinterpret_cast<std::uintptr_t>(aligned) % 4 == 0u);
            PPR_TEST_ASSERT(aligned > unaligned);

            auto *already = alignForward(data, std::align_val_t{alignof(int)});
            PPR_TEST_ASSERT(already == data);
        };

        PPR_UNIT_TEST(align_pointer_backward) {
            int data[16]{};
            const auto base = reinterpret_cast<std::uintptr_t>(data);
            auto *unaligned = reinterpret_cast<int *>(base + 5);
            auto *aligned = alignBackward(unaligned, std::align_val_t{4});
            PPR_TEST_ASSERT(reinterpret_cast<std::uintptr_t>(aligned) % 4 == 0u);
            PPR_TEST_ASSERT(aligned < unaligned);

            auto *already = alignBackward(data, std::align_val_t{alignof(int)});
            PPR_TEST_ASSERT(already == data);
        };

        PPR_UNIT_TEST(alignment_traits) {
            static_assert(alignof_v<int> == std::align_val_t{alignof(int)});
            static_assert(alignof_v<double> == std::align_val_t{alignof(double)});
            static_assert(alignof_v<void *> == std::align_val_t{alignof(void *)});
            static_assert(alignof_v<u64> == std::align_val_t{alignof(u64)});
            PPR_TEST_ASSERT(static_cast<std::size_t>(max_align_v) >= alignof(std::max_align_t));
        };

        PPR_UNIT_TEST(bit_count) {
            static_assert(bit_count_v<int> == 32u);
            static_assert(bit_count_v<u64> == 64u);
            static_assert(bit_count_v<char> == 8u);
            static_assert(bit_count_v<u8> == 8u);
            static_assert(bit_count_v<u16> == 16u);
            static_assert(bit_count_v<u32> == 32u);

            static_assert(bit_count_v<const int &> == 32u);
            static_assert(bit_count_v<int &&> == 32u);
        };

        PPR_UNIT_TEST(static_iota_sum) {
            constexpr auto sum = []<std::size_t... Is>(std::integral_constant<std::size_t, Is>...) {
                return (0u + ... + Is);
            };
            constexpr auto r = static_iota<5u>(sum);
            PPR_TEST_ASSERT(r == 0u + 1u + 2u + 3u + 4u);
        };

        PPR_UNIT_TEST(static_iota_typed) {
            constexpr auto sum = []<int... Is>(std::integral_constant<int, Is>...) {
                return (0 + ... + Is);
            };
            constexpr auto r = static_iota<int, 5>(sum);
            PPR_TEST_ASSERT(r == 0 + 1 + 2 + 3 + 4);
        };

        PPR_UNIT_TEST(static_iota_empty) {
            constexpr auto sum = []<std::size_t... Is>(std::integral_constant<std::size_t, Is>...) {
                return (0u + ... + Is);
            };
            constexpr auto r = static_iota<0u>(sum);
            PPR_TEST_ASSERT(r == 0u);
        };

        PPR_UNIT_TEST(static_iota_fill_array) {
            constexpr auto arr = []() {
                std::array<int, 4> result{};
                static_iota<4u>([&result]<std::size_t... Is>(std::integral_constant<std::size_t, Is>...) {
                    ((result[Is] = static_cast<int>(Is)), ...);
                });
                return result;
            }();
            PPR_TEST_ASSERT(arr[0] == 0);
            PPR_TEST_ASSERT(arr[1] == 1);
            PPR_TEST_ASSERT(arr[2] == 2);
            PPR_TEST_ASSERT(arr[3] == 3);
        };

        PPR_UNIT_TEST(static_iota_compile_types) {
            constexpr auto check = []<std::size_t... Is>(std::integral_constant<std::size_t, Is>...) {
                return sizeof...(Is);
            };
            constexpr auto r = static_iota<3u>(check);
            PPR_TEST_ASSERT(r == 3u);
        };

        PPR_UNIT_TEST(shuffle_context_seed) {
            UnitTest::Context ctx{};
            PPR_TEST_ASSERT(not ctx.m_shuffle_seed.has_value());

            ctx.m_shuffle_seed = 42u;
            PPR_TEST_ASSERT(ctx.m_shuffle_seed.has_value());
            PPR_TEST_ASSERT(*ctx.m_shuffle_seed == 42u);
        };

        PPR_UNIT_TEST(errc_not_found_is_error) {
            const auto ec = make_error_code(std::errc::no_such_device);
            PPR_TEST_ASSERT(!!ec);
            PPR_TEST_ASSERT(ec.value() == enumOrd(std::errc::no_such_device));
        };

        PPR_UNIT_TEST(failed_ec_zero_returns_false) {
            std::error_code ec{};
            PPR_TEST_ASSERT(not hasFailed(ec));
        };

        PPR_UNIT_TEST(failed_ec_nonzero_returns_true) {
            std::error_code ec{42, std::generic_category()};
            PPR_TEST_ASSERT(hasFailed(ec));
        };
    }

    PPR_UNIT_TEST(utility) {
        _.recurse({
            Utility::divide_round_up_basic,
            Utility::align_backward_basic,
            Utility::align_forward_basic,
            Utility::align_forward_align_val,
            Utility::align_pointer_forward,
            Utility::align_pointer_backward,
            Utility::alignment_traits,
            Utility::bit_count,
            Utility::static_iota_sum,
            Utility::static_iota_typed,
            Utility::static_iota_empty,
            Utility::static_iota_fill_array,
            Utility::static_iota_compile_types,
            Utility::shuffle_context_seed,
            Utility::errc_not_found_is_error,
            Utility::failed_ec_zero_returns_false,
            Utility::failed_ec_nonzero_returns_true,
        });
    };
}
