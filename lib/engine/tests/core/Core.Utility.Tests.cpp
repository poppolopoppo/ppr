module;
#include "pP/UnitTest.h"

module engine.tests.core;

import engine.core;
import std;

namespace pP::tests::detail {
    namespace Utility {
        PPR_UNIT_TEST (command_history_owns_values_and_preserves_failed_operations) {
            struct Change {
                std::string m_name;
                int m_amount{};
            };
            CommandHistory<Change> history{8u, 64u};
            int value = 0;
            bool fail = false;
            const auto apply = [&](const Change &change) -> std::error_code {
                if (fail) {
                    return std::make_error_code(std::errc::operation_canceled);
                }
                value += change.m_amount;
                return {};
            };
            const auto revert = [&](const Change &change) -> std::error_code {
                if (fail) {
                    return std::make_error_code(std::errc::operation_canceled);
                }
                value -= change.m_amount;
                return {};
            };
            PPR_TEST_ASSERT(history.undo(revert).value() == false);
            PPR_TEST_ASSERT(history.redo(apply).value() == false);
            Change first{"owned", 3};
            PPR_TEST_ASSERT(not history.execute(first, first.m_name.size(), apply));
            first.m_name = "changed";
            first.m_amount = 900;
            PPR_TEST_ASSERT(value == 3 and history.canUndo() and not history.canRedo());
            fail = true;
            const auto failed_undo = history.undo(revert);
            PPR_TEST_ASSERT(not failed_undo and failed_undo.error() == std::errc::operation_canceled);
            PPR_TEST_ASSERT(value == 3 and history.canUndo() and not history.canRedo());
            fail = false;
            PPR_TEST_ASSERT(history.undo(revert).value() and value == 0 and history.canRedo());
            fail = true;
            const auto failed_redo = history.redo(apply);
            PPR_TEST_ASSERT(not failed_redo and failed_redo.error() == std::errc::operation_canceled);
            PPR_TEST_ASSERT(history.execute(Change{"failed", 10}, 6u, apply) == std::errc::operation_canceled);
            PPR_TEST_ASSERT(value == 0 and history.canRedo() and not history.canUndo());
            fail = false;
            PPR_TEST_ASSERT(history.redo(apply).value() and value == 3);
            PPR_TEST_ASSERT(history.undo(revert).value() and value == 0);
            PPR_TEST_ASSERT(not history.execute(Change{"branch", 5}, 6u, apply));
            PPR_TEST_ASSERT(value == 5 and not history.canRedo());
            PPR_TEST_ASSERT(history.undo(revert).value() and value == 0);
            history.clear();
            PPR_TEST_ASSERT(not history.canUndo() and not history.canRedo() and history.payloadBytes() == 0u);
        };

        PPR_UNIT_TEST (command_history_evicts_oldest_entries_and_rejects_oversized_before_apply) {
            CommandHistory<int> history{2u, 5u};
            int value = 0;
            const auto apply = [&](const int delta) -> std::error_code {
                value += delta;
                return {};
            };
            const auto revert = [&](const int delta) -> std::error_code {
                value -= delta;
                return {};
            };
            PPR_TEST_ASSERT(not history.execute(1, 2u, apply));
            PPR_TEST_ASSERT(not history.execute(2, 2u, apply));
            PPR_TEST_ASSERT(not history.execute(3, 2u, apply));
            PPR_TEST_ASSERT(value == 6 and history.payloadBytes() == 4u);
            PPR_TEST_ASSERT(history.undo(revert).value() and history.undo(revert).value());
            PPR_TEST_ASSERT(value == 1 and not history.undo(revert).value());
            PPR_TEST_ASSERT(not history.execute(7, 5u, apply));
            PPR_TEST_ASSERT(value == 8 and history.payloadBytes() == 5u and not history.canRedo());
            PPR_TEST_ASSERT(history.execute(100, 6u, apply) == std::errc::no_buffer_space);
            PPR_TEST_ASSERT(value == 8 and history.payloadBytes() == 5u);
            PPR_TEST_ASSERT(history.undo(revert).value() and value == 1 and not history.canUndo());
            CommandHistory<int> disabled{0u, 5u};
            PPR_TEST_ASSERT(disabled.execute(100, 1u, apply) == std::errc::no_buffer_space and value == 1);

            CommandHistory<int> payload_limited{10u, 3u};
            PPR_TEST_ASSERT(not payload_limited.execute(4, 2u, apply));
            PPR_TEST_ASSERT(not payload_limited.execute(5, 2u, apply));
            PPR_TEST_ASSERT(payload_limited.payloadBytes() == 2u);
            PPR_TEST_ASSERT(payload_limited.undo(revert).value() and value == 5);
            PPR_TEST_ASSERT(not payload_limited.undo(revert).value());
        };

        PPR_UNIT_TEST (divide_round_up_basic) {
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

        PPR_UNIT_TEST (align_backward_basic) {
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

        PPR_UNIT_TEST (align_forward_basic) {
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

        PPR_UNIT_TEST (align_forward_align_val) {
            constexpr auto a1 = alignForward(10u, std::align_val_t{8});
            PPR_TEST_ASSERT(a1 == 16u);

            constexpr auto a2 = alignForward(8u, std::align_val_t{8});
            PPR_TEST_ASSERT(a2 == 8u);

            constexpr auto a3 = alignForward(0u, std::align_val_t{8});
            PPR_TEST_ASSERT(a3 == 0u);

            constexpr auto a4 = alignForward(1u, std::align_val_t{16});
            PPR_TEST_ASSERT(a4 == 16u);
        };

        PPR_UNIT_TEST (align_pointer_forward) {
            int data[16]{};
            const auto base = reinterpret_cast<std::uintptr_t>(data);
            auto *unaligned = reinterpret_cast<int *>(base + 1);
            auto *aligned = alignForward(unaligned, std::align_val_t{4});
            PPR_TEST_ASSERT(reinterpret_cast<std::uintptr_t>(aligned) % 4 == 0u);
            PPR_TEST_ASSERT(aligned > unaligned);

            auto *already = alignForward(data, std::align_val_t{alignof(int)});
            PPR_TEST_ASSERT(already == data);
        };

        PPR_UNIT_TEST (align_pointer_backward) {
            int data[16]{};
            const auto base = reinterpret_cast<std::uintptr_t>(data);
            auto *unaligned = reinterpret_cast<int *>(base + 5);
            auto *aligned = alignBackward(unaligned, std::align_val_t{4});
            PPR_TEST_ASSERT(reinterpret_cast<std::uintptr_t>(aligned) % 4 == 0u);
            PPR_TEST_ASSERT(aligned < unaligned);

            auto *already = alignBackward(data, std::align_val_t{alignof(int)});
            PPR_TEST_ASSERT(already == data);
        };

        PPR_UNIT_TEST (alignment_traits) {
            static_assert(alignof_v<int> == std::align_val_t{alignof(int)});
            static_assert(alignof_v<double> == std::align_val_t{alignof(double)});
            static_assert(alignof_v<void *> == std::align_val_t{alignof(void *)});
            static_assert(alignof_v<u64> == std::align_val_t{alignof(u64)});
            PPR_TEST_ASSERT(static_cast<std::size_t>(max_align_v) >= alignof(std::max_align_t));
        };

        PPR_UNIT_TEST (bit_count) {
            static_assert(bit_count_v<int> == 32u);
            static_assert(bit_count_v<u64> == 64u);
            static_assert(bit_count_v<char> == 8u);
            static_assert(bit_count_v<u8> == 8u);
            static_assert(bit_count_v<u16> == 16u);
            static_assert(bit_count_v<u32> == 32u);

            static_assert(bit_count_v<const int &> == 32u);
            static_assert(bit_count_v<int &&> == 32u);
        };

        PPR_UNIT_TEST (static_iota_sum) {
            constexpr auto sum = []<std::size_t... Is>(
                                     std::integral_constant<std::size_t, Is>...) { return (0u + ... + Is); };
            constexpr auto r = static_iota<5u>(sum);
            PPR_TEST_ASSERT(r == 0u + 1u + 2u + 3u + 4u);
        };

        PPR_UNIT_TEST (static_iota_typed) {
            constexpr auto sum = []<int... Is>(std::integral_constant<int, Is>...) { return (0 + ... + Is); };
            constexpr auto r = static_iota<int, 5>(sum);
            PPR_TEST_ASSERT(r == 0 + 1 + 2 + 3 + 4);
        };

        PPR_UNIT_TEST (static_iota_empty) {
            constexpr auto sum = []<std::size_t... Is>(
                                     std::integral_constant<std::size_t, Is>...) { return (0u + ... + Is); };
            constexpr auto r = static_iota<0u>(sum);
            PPR_TEST_ASSERT(r == 0u);
        };

        PPR_UNIT_TEST (static_iota_fill_array) {
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

        PPR_UNIT_TEST (static_iota_compile_types) {
            constexpr auto check = []<std::size_t... Is>(
                                       std::integral_constant<std::size_t, Is>...) { return sizeof...(Is); };
            constexpr auto r = static_iota<3u>(check);
            PPR_TEST_ASSERT(r == 3u);
        };

        PPR_UNIT_TEST (shuffle_context_seed) {
            UnitTest::Context ctx{};
            PPR_TEST_ASSERT(not ctx.m_shuffle_seed.has_value());

            ctx.m_shuffle_seed = 42u;
            PPR_TEST_ASSERT(ctx.m_shuffle_seed.has_value());
            PPR_TEST_ASSERT(*ctx.m_shuffle_seed == 42u);
        };

        PPR_UNIT_TEST (errc_not_found_is_error) {
            const auto ec = make_error_code(std::errc::no_such_device);
            PPR_TEST_ASSERT(!!ec);
            PPR_TEST_ASSERT(ec.value() == enumOrd(std::errc::no_such_device));
        };

        PPR_UNIT_TEST (failed_ec_zero_returns_false) {
            std::error_code ec{};
            PPR_TEST_ASSERT(not hasFailed(ec));
        };

        PPR_UNIT_TEST (failed_ec_nonzero_returns_true) {
            std::error_code ec{42, std::generic_category()};
            PPR_TEST_ASSERT(hasFailed(ec));
        };

        PPR_UNIT_TEST (percentile_empty_nullopt) {
            std::array<float, 4> scratch{1.0f, 2.0f, 3.0f, 4.0f};
            const std::optional<float> result = percentile_nearest_rank({}, std::span<float>{scratch}, 0.5f);
            PPR_TEST_ASSERT(not result.has_value());
        };

        PPR_UNIT_TEST (percentile_singleton) {
            const std::array<float, 1> samples{7.0f};
            std::array<float, 1> scratch{};
            for (const float q: {0.0f, 0.5f, 0.95f, 1.0f}) {
                const std::optional<float> result = percentile_nearest_rank(samples, std::span<float>{scratch}, q);
                PPR_TEST_ASSERT(result.has_value());
                PPR_TEST_ASSERT(*result == 7.0f);
            }
        };

        PPR_UNIT_TEST (percentile_odd_unsorted) {
            const std::array<float, 5> samples{5.0f, 1.0f, 4.0f, 2.0f, 3.0f};
            std::array<float, 5> scratch{};
            const std::optional<float> median = percentile_nearest_rank(samples, std::span<float>{scratch}, 0.5f);
            PPR_TEST_ASSERT(median.has_value());
            PPR_TEST_ASSERT(*median == 3.0f);
            // The scratch copy sorts in place: [1, 2, 3, 4, 5].
            PPR_TEST_ASSERT(scratch[0] == 1.0f);
            PPR_TEST_ASSERT(scratch[4] == 5.0f);
        };

        PPR_UNIT_TEST (percentile_even_ranks) {
            const std::array<float, 4> samples{4.0f, 1.0f, 3.0f, 2.0f};
            std::array<float, 4> scratch{};
            // Sorted [1, 2, 3, 4]: ceil(25*4/100)-1 = 0, ceil(50*4/100)-1 = 1,
            // ceil(75*4/100)-1 = 2, ceil(95*4/100)-1 = 3.
            for (const auto [q, want]: {std::pair{0.25f, 1.0f}, {0.5f, 2.0f}, {0.75f, 3.0f}, {0.95f, 4.0f}}) {
                const std::optional<float> result = percentile_nearest_rank(samples, std::span<float>{scratch}, q);
                PPR_TEST_ASSERT(result.has_value());
                PPR_TEST_ASSERT(*result == want);
            }
        };

        PPR_UNIT_TEST (percentile_duplicates) {
            const std::array<float, 4> samples{3.0f, 1.0f, 3.0f, 1.0f};
            std::array<float, 4> scratch{};
            // Sorted [1, 1, 3, 3]: p50 lands on index 1, p95 on index 3.
            const std::optional<float> half = percentile_nearest_rank(samples, std::span<float>{scratch}, 0.5f);
            PPR_TEST_ASSERT(half.has_value());
            PPR_TEST_ASSERT(*half == 1.0f);
            const std::optional<float> high = percentile_nearest_rank(samples, std::span<float>{scratch}, 0.95f);
            PPR_TEST_ASSERT(high.has_value());
            PPR_TEST_ASSERT(*high == 3.0f);
        };

        PPR_UNIT_TEST (percentile_boundaries_clamp) {
            const std::array<float, 3> samples{9.0f, 7.0f, 8.0f};
            std::array<float, 3> scratch{};
            // q = 0 names the minimum, q = 1 the maximum; out-of-range
            // fractions clamp to the same ends.
            for (const auto [q, want]: {std::pair{0.0f, 7.0f}, {-1.0f, 7.0f}, {1.0f, 9.0f}, {2.0f, 9.0f}}) {
                const std::optional<float> result = percentile_nearest_rank(samples, std::span<float>{scratch}, q);
                PPR_TEST_ASSERT(result.has_value());
                PPR_TEST_ASSERT(*result == want);
            }
        };

        PPR_UNIT_TEST (percentile_oversized_scratch) {
            const std::array<float, 3> samples{9.0f, 7.0f, 8.0f};
            std::array<float, 8> scratch{};
            scratch[7] = -1.0f;
            // Sorted [7, 8, 9]: ceil(50*3/100)-1 = 1.
            const std::optional<float> result = percentile_nearest_rank(samples, std::span<float>{scratch}, 0.5f);
            PPR_TEST_ASSERT(result.has_value());
            PPR_TEST_ASSERT(*result == 8.0f);
            // Elements past the sample count are never touched.
            PPR_TEST_ASSERT(scratch[7] == -1.0f);
        };
    }
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest utility = UnitTest::Named("utility") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::Utility::command_history_owns_values_and_preserves_failed_operations,
            detail::Utility::command_history_evicts_oldest_entries_and_rejects_oversized_before_apply,
            detail::Utility::divide_round_up_basic,
            detail::Utility::align_backward_basic,
            detail::Utility::align_forward_basic,
            detail::Utility::align_forward_align_val,
            detail::Utility::align_pointer_forward,
            detail::Utility::align_pointer_backward,
            detail::Utility::alignment_traits,
            detail::Utility::bit_count,
            detail::Utility::static_iota_sum,
            detail::Utility::static_iota_typed,
            detail::Utility::static_iota_empty,
            detail::Utility::static_iota_fill_array,
            detail::Utility::static_iota_compile_types,
            detail::Utility::shuffle_context_seed,
            detail::Utility::errc_not_found_is_error,
            detail::Utility::failed_ec_zero_returns_false,
            detail::Utility::failed_ec_nonzero_returns_true,
            detail::Utility::percentile_empty_nullopt,
            detail::Utility::percentile_singleton,
            detail::Utility::percentile_odd_unsorted,
            detail::Utility::percentile_even_ranks,
            detail::Utility::percentile_duplicates,
            detail::Utility::percentile_boundaries_clamp,
            detail::Utility::percentile_oversized_scratch,
        });
    };

    const UnitTest &utilityTests() noexcept {
        return utility;
    }
} // namespace pP::tests
