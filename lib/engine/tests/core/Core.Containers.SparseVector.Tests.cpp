module;
#include "pP/UnitTest.h"

module engine.tests.core;

import engine.core;
import std;

namespace pP::tests::detail {
    namespace Sparse_vector {
        PPR_UNIT_TEST (allocation_and_free_list) {
            pP::SparseVector<int> vec;
            vec.reserveAssumeEmpty(8u);

            const auto key1 = vec.add(10);
            const auto key2 = vec.add(20);
            PPR_TEST_ASSERT(vec.size() == 2);
            PPR_TEST_ASSERT(vec[key1] == 10);
            PPR_TEST_ASSERT(vec[key2] == 20);

            for (int i = 0u; vec.size() < vec.capacity(); ++i) {
                vec.add(i);
            }

            vec.erase(key1);
            PPR_TEST_ASSERT(vec.size() == vec.capacity() - 1u);

            const auto key3 = vec.add(30);
            PPR_TEST_ASSERT(key3.m_index == key1.m_index);
            PPR_TEST_ASSERT(key3.m_seed != key1.m_seed);
            PPR_TEST_ASSERT(vec[key3] == 30);
        };

        PPR_UNIT_TEST (jump_counting_logic) {
            pP::SparseVector<int> vec;
            [[maybe_unused]] const auto k0 = vec.add(0);
            const auto k1 = vec.add(1);
            const auto k2 = vec.add(2);
            const auto k3 = vec.add(3);
            [[maybe_unused]] const auto k4 = vec.add(4);

            vec.erase(k1);
            vec.erase(k2);
            vec.erase(k3);

            auto it = vec.begin();
            PPR_TEST_ASSERT(it.getIndex() == 0);
            ++it;
            PPR_TEST_ASSERT(it.getIndex() == 4);

            --it;
            PPR_TEST_ASSERT(it.getIndex() == 0);
        };

        PPR_UNIT_TEST (key_validation) {
            pP::SparseVector<int> vec;
            auto key = vec.add(100);

            PPR_TEST_ASSERT(vec.contains(key));

            auto it = vec.find(100);
            PPR_TEST_ASSERT(it != vec.end());
            PPR_TEST_ASSERT(*it == 100);

            auto optional_value = vec.tryGet(key);
            PPR_TEST_ASSERT(optional_value != nullptr);
            PPR_TEST_ASSERT(*optional_value == 100);
            vec.erase(key);

            PPR_TEST_ASSERT(!vec.contains(key));
        };

        PPR_UNIT_TEST (copy_and_move) {
            pP::SparseVector<int> vec;
            vec.emplace(1);
            vec.emplace(2);

            pP::SparseVector<int> copy_vec(vec);
            PPR_TEST_ASSERT(copy_vec.size() == 2);

            pP::SparseVector<int> move_vec(std::move(copy_vec));
            PPR_TEST_ASSERT(move_vec.size() == 2);
            PPR_TEST_ASSERT(copy_vec.isEmpty());
        };

        PPR_UNIT_TEST (iteration_boundary) {
            pP::SparseVector<int> vec;
            PPR_TEST_ASSERT(vec.begin() == vec.end());

            vec.emplace(1);
            auto it = vec.begin();
            PPR_TEST_ASSERT(it != vec.end());
            ++it;
            PPR_TEST_ASSERT(it == vec.end());
        };

        PPR_UNIT_TEST (memory_stability) {
            pP::SparseVector<int> vec;
            vec.reserve(10);

            int *ptrs[10];
            SparseKeyId keys[10];

            for (int i = 0; i < 10; ++i) {
                auto res = vec.emplace(i);
                ptrs[i] = res.getPointer();
                keys[i] = res.getKey();
            }

            vec.reserve(100);

            for (int i = 0; i < 10; ++i) {
                PPR_TEST_ASSERT(vec[keys[i]] == i);
                PPR_TEST_ASSERT(&vec[keys[i]] == ptrs[i]);
            }
        };

        PPR_UNIT_TEST (default_key_is_invalid) {
            constexpr pP::SparseKeyId default_key{};
            PPR_TEST_ASSERT(!default_key.isValid());
            PPR_TEST_ASSERT(default_key.m_seed == 0u);

            constexpr pP::SparseKeyId value_init = pP::SparseKeyId();
            PPR_TEST_ASSERT(!value_init.isValid());

            const pP::SparseKeyId invalid_from_default = pP::default_value_v;
            PPR_TEST_ASSERT(!invalid_from_default.isValid());
        };

        PPR_UNIT_TEST (add_yields_valid_key) {
            pP::SparseVector<int> vec;
            const auto key = vec.add(42);
            PPR_TEST_ASSERT(key.isValid());
            PPR_TEST_ASSERT(key.m_seed != 0u);
            PPR_TEST_ASSERT(vec.contains(key));
            PPR_TEST_ASSERT(vec[key] == 42);
        };

        PPR_UNIT_TEST (erased_key_fails_closed) {
            pP::SparseVector<int> vec;
            const auto key = vec.add(7);
            PPR_TEST_ASSERT(vec.erase(key));
            PPR_TEST_ASSERT(!vec.contains(key));
            PPR_TEST_ASSERT(vec.tryGet(key) == nullptr);
            PPR_TEST_ASSERT(!vec.erase(key));
        };

        PPR_UNIT_TEST (seed_reuse_invalidates_stale) {
            pP::SparseVector<int> vec;
            vec.reserveAssumeEmpty(8u);
            const auto stale_key = vec.add(10);
            // fill to capacity so the freed slot is recycled on next add
            for (int i = 0u; vec.size() < vec.capacity(); ++i) {
                vec.add(i);
            }
            PPR_TEST_ASSERT(vec.erase(stale_key));
            const auto fresh_key = vec.add(30);
            PPR_TEST_ASSERT(fresh_key.m_index == stale_key.m_index);
            PPR_TEST_ASSERT(fresh_key.m_seed != stale_key.m_seed);
            PPR_TEST_ASSERT(fresh_key.isValid());
            PPR_TEST_ASSERT(!vec.contains(stale_key));
            PPR_TEST_ASSERT(vec.tryGet(stale_key) == nullptr);
            PPR_TEST_ASSERT(!vec.erase(stale_key));
            PPR_TEST_ASSERT(vec.contains(fresh_key));
            PPR_TEST_ASSERT(vec[fresh_key] == 30);
        };

        PPR_UNIT_TEST (key_ordering_invalid_vs_valid) {
            pP::SparseVector<int> vec;
            const auto valid_key = vec.add(1);
            constexpr pP::SparseKeyId invalid_key{};
            PPR_TEST_ASSERT(!invalid_key.isValid());
            PPR_TEST_ASSERT(valid_key.isValid());
            PPR_TEST_ASSERT((valid_key <=> invalid_key) == std::strong_ordering::less);
            PPR_TEST_ASSERT((invalid_key <=> valid_key) == std::strong_ordering::greater);
            PPR_TEST_ASSERT((invalid_key <=> pP::SparseKeyId{}) == std::strong_ordering::equal);
            PPR_TEST_ASSERT(valid_key == valid_key);
            PPR_TEST_ASSERT(!(valid_key == invalid_key));
        };

        PPR_UNIT_TEST (numeric_handle_capabilities) {
            struct SparseKeyTag {
            };
            struct U32Tag {
            };
            using SparseHandle = pP::Numeric<pP::SparseKeyId, SparseKeyTag>;
            using U32Handle = pP::Numeric<pP::u32, U32Tag>;

            static_assert(std::is_standard_layout_v<pP::SparseKeyId>);
            static_assert(sizeof(pP::SparseKeyId) == 4u);
            static_assert(std::is_standard_layout_v<SparseHandle>);
            static_assert(sizeof(SparseHandle) == 4u);
            static_assert(std::is_standard_layout_v<U32Handle>);
            static_assert(sizeof(U32Handle) == 4u);
            static_assert(std::equality_comparable<SparseHandle>);
            static_assert(std::three_way_comparable<SparseHandle>);
            static_assert(std::equality_comparable<U32Handle>);
            static_assert(std::three_way_comparable<U32Handle>);
            static_assert(pP::hash::THashable<SparseHandle>);
            static_assert(pP::hash::THashable<U32Handle>);

            constexpr SparseHandle default_handle{};
            constexpr U32Handle default_u32{};
            PPR_TEST_ASSERT(!(*default_handle).isValid());
            PPR_TEST_ASSERT(*default_u32 == 0u);

            constexpr SparseHandle copy_handle{default_handle.m_value};
            PPR_TEST_ASSERT(copy_handle == default_handle);
            PPR_TEST_ASSERT(!(copy_handle != default_handle));

            PPR_TEST_ASSERT(pP::hashValue(default_handle) == pP::hashValue(copy_handle));
            PPR_TEST_ASSERT(pP::hashValue(default_u32) == pP::hashValue(U32Handle{}));
        };

        PPR_UNIT_TEST (sparse_handle_capabilities) {
            struct HandleTag {
            };
            using Handle64 = pP::Numeric<pP::SparseHandle, HandleTag>;

            static_assert(std::is_standard_layout_v<pP::SparseHandle>);
            static_assert(std::is_trivially_copyable_v<pP::SparseHandle>);
            static_assert(sizeof(pP::SparseHandle) == 8u);
            static_assert(std::is_standard_layout_v<Handle64>);
            static_assert(sizeof(Handle64) == 8u);
            static_assert(std::equality_comparable<pP::SparseHandle>);
            static_assert(std::three_way_comparable<pP::SparseHandle>);
            static_assert(std::equality_comparable<Handle64>);
            static_assert(std::three_way_comparable<Handle64>);
            static_assert(pP::hash::THashable<Handle64>);

            constexpr pP::SparseHandle default_handle{};
            static_assert(!default_handle.isValid());
            PPR_TEST_ASSERT(!default_handle.isValid());
            PPR_TEST_ASSERT(default_handle.m_generation == 0u);

            constexpr pP::SparseHandle live_handle{3u, 7u};
            static_assert(live_handle.isValid());
            PPR_TEST_ASSERT(live_handle.isValid());
            PPR_TEST_ASSERT(live_handle.m_index == 3u);
            PPR_TEST_ASSERT(live_handle.m_generation == 7u);

            PPR_TEST_ASSERT((live_handle <=> default_handle) == std::strong_ordering::less);
            PPR_TEST_ASSERT((default_handle <=> live_handle) == std::strong_ordering::greater);
            PPR_TEST_ASSERT((default_handle <=> pP::SparseHandle{}) == std::strong_ordering::equal);
            PPR_TEST_ASSERT(hashValue(live_handle) == hashValue(pP::SparseHandle{3u, 7u}));
            PPR_TEST_ASSERT(hashValue(live_handle) != hashValue(pP::SparseHandle{3u, 8u}));

            constexpr Handle64 default_numeric{};
            PPR_TEST_ASSERT(!(*default_numeric).isValid());
            constexpr Handle64 copy_numeric{default_numeric.m_value};
            PPR_TEST_ASSERT(copy_numeric == default_numeric);
            PPR_TEST_ASSERT(pP::hashValue(default_numeric) == pP::hashValue(copy_numeric));

            const pP::SparseHandle invalid_from_default = pP::default_value_v;
            PPR_TEST_ASSERT(!invalid_from_default.isValid());
        };

        PPR_UNIT_TEST (handle_lifecycle_fail_closed) {
            pP::SparseVector<int> vec;
            vec.reserveAssumeEmpty(8u);
            const auto live = vec.addHandle(42);
            PPR_TEST_ASSERT(live.isValid());
            PPR_TEST_ASSERT(live.m_index == 0u);
            // Fill to capacity so the erased slot below is recycled on next add.
            while (vec.size() < vec.capacity()) {
                vec.addHandle(0);
            }
            PPR_TEST_ASSERT(vec.contains(live));
            PPR_TEST_ASSERT(vec.tryGet(live) != nullptr);
            PPR_TEST_ASSERT(*vec.tryGet(live) == 42);
            PPR_TEST_ASSERT(vec.get(live) == 42);

            PPR_TEST_ASSERT(vec.handle(0u) == live);
            auto it = vec.begin();
            PPR_TEST_ASSERT(it.getHandle() == live);
            PPR_TEST_ASSERT(pP::SparseVector<int>::handle(it) == live);

            constexpr pP::SparseHandle invalid{};
            PPR_TEST_ASSERT(!vec.contains(invalid));
            PPR_TEST_ASSERT(vec.tryGet(invalid) == nullptr);
            PPR_TEST_ASSERT(!vec.erase(invalid));

            PPR_TEST_ASSERT(vec.erase(live));
            PPR_TEST_ASSERT(!vec.contains(live));
            PPR_TEST_ASSERT(vec.tryGet(live) == nullptr);
            PPR_TEST_ASSERT(!vec.erase(live));

            const auto fresh = vec.addHandle(7);
            PPR_TEST_ASSERT(fresh.isValid());
            PPR_TEST_ASSERT(fresh.m_index == live.m_index);
            PPR_TEST_ASSERT(fresh.m_generation != live.m_generation);
            PPR_TEST_ASSERT(!vec.contains(live));
            PPR_TEST_ASSERT(vec.tryGet(live) == nullptr);
            PPR_TEST_ASSERT(vec.contains(fresh));
            PPR_TEST_ASSERT(vec[fresh] == 7);
        };

        PPR_UNIT_TEST (handle_wrap_stress_no_aba) {
            pP::SparseVector<int> vec;
            vec.reserveAssumeEmpty(8u);
            const pP::SparseHandle first = vec.addHandle(0);
            PPR_TEST_ASSERT(first.m_index == 0u);
            // Fill to capacity, then free slot 0: every add below recycles the
            // same logical slot, exercising 100,000 reuse generations there.
            while (vec.size() < vec.capacity()) {
                vec.addHandle(-1);
            }
            PPR_TEST_ASSERT(vec.erase(first));

            std::vector<pP::SparseHandle> history;
            history.reserve(100000u);

            for (u32 i = 0u; i < 100000u; ++i) {
                const pP::SparseHandle live = vec.addHandle(static_cast<int>(i));
                PPR_TEST_ASSERT(live.isValid());
                PPR_TEST_ASSERT(live.m_index == 0u);
                history.push_back(live);
                PPR_TEST_ASSERT(vec.erase(live));
            }
            PPR_TEST_ASSERT(history.size() == 100000u);

            for (u32 i = 1u; i < history.size(); ++i) {
                PPR_TEST_ASSERT(history[i].m_generation == history[i - 1u].m_generation + 1u);
            }

            for (u32 i = 0u; i < history.size(); ++i) {
                PPR_TEST_ASSERT(!vec.contains(history[i]));
                PPR_TEST_ASSERT(vec.tryGet(history[i]) == nullptr);
                PPR_TEST_ASSERT(!vec.erase(history[i]));
            }

            const pP::SparseHandle tail = vec.addHandle(999999);
            PPR_TEST_ASSERT(tail.isValid());
            PPR_TEST_ASSERT(tail.m_index == 0u);
            PPR_TEST_ASSERT(vec.contains(tail));
            PPR_TEST_ASSERT(vec.tryGet(tail) != nullptr);
            PPR_TEST_ASSERT(*vec.tryGet(tail) == 999999);
        };
    }
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest sparse_vector = UnitTest::Named("sparse_vector") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::Sparse_vector::allocation_and_free_list,
            detail::Sparse_vector::jump_counting_logic,
            detail::Sparse_vector::key_validation,
            detail::Sparse_vector::copy_and_move,
            detail::Sparse_vector::iteration_boundary,
            detail::Sparse_vector::memory_stability,
            detail::Sparse_vector::default_key_is_invalid,
            detail::Sparse_vector::add_yields_valid_key,
            detail::Sparse_vector::erased_key_fails_closed,
            detail::Sparse_vector::seed_reuse_invalidates_stale,
            detail::Sparse_vector::key_ordering_invalid_vs_valid,
            detail::Sparse_vector::numeric_handle_capabilities,
            detail::Sparse_vector::sparse_handle_capabilities,
            detail::Sparse_vector::handle_lifecycle_fail_closed,
            detail::Sparse_vector::handle_wrap_stress_no_aba,
        });
    };

    const UnitTest &sparse_vectorTests() noexcept {
        return sparse_vector;
    }
} // namespace pP::tests
