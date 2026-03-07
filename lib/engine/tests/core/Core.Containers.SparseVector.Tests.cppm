module;
#include "pP/Macros.h"
export module engine.tests:core_sparse_vector;
import std;
import engine.core;

export namespace pP::tests {
    namespace SparseVector {
        PPR_UNIT_TEST(allocation_and_free_list) {
            pP::SparseVector<int> vec;
            vec.reserveAssumeEmpty(8u);

            const auto key1 = vec.add(10);
            const auto key2 = vec.add(20);
            PPR_ASSERT(vec.size() == 2);
            PPR_ASSERT(vec[key1] == 10);
            PPR_ASSERT(vec[key2] == 20);

            for (int i = 0u; vec.size() < vec.capacity(); ++i) {
                vec.add(i);
            }

            vec.erase(key1);
            PPR_ASSERT(vec.size() == vec.capacity() - 1u);

            const auto key3 = vec.add(30);
            PPR_ASSERT(key3.m_index == key1.m_index);
            PPR_ASSERT(key3.m_seed != key1.m_seed);
            PPR_ASSERT(vec[key3] == 30);
        };

        PPR_UNIT_TEST(jump_counting_logic) {
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
            PPR_ASSERT(it.getIndex() == 0);
            ++it;
            PPR_ASSERT(it.getIndex() == 4);

            --it;
            PPR_ASSERT(it.getIndex() == 0);
        };

        PPR_UNIT_TEST(key_validation) {
            pP::SparseVector<int> vec;
            auto key = vec.add(100);

            PPR_ASSERT(vec.contains(key));

            auto it = vec.find(100);
            PPR_ASSERT(it != vec.end());
            PPR_ASSERT(*it == 100);

            auto optional_value = vec.tryGet(key);
            PPR_ASSERT(optional_value != nullptr);
            PPR_ASSERT(*optional_value == 100);
            vec.erase(key);

            PPR_ASSERT(!vec.contains(key));
        };

        PPR_UNIT_TEST(copy_and_move) {
            pP::SparseVector<int> vec;
            vec.emplace(1);
            vec.emplace(2);

            pP::SparseVector<int> copy_vec(vec);
            PPR_ASSERT(copy_vec.size() == 2);

            pP::SparseVector<int> move_vec(std::move(copy_vec));
            PPR_ASSERT(move_vec.size() == 2);
            PPR_ASSERT(copy_vec.isEmpty());
        };

        PPR_UNIT_TEST(iteration_boundary) {
            pP::SparseVector<int> vec;
            PPR_ASSERT(vec.begin() == vec.end());

            vec.emplace(1);
            auto it = vec.begin();
            PPR_ASSERT(it != vec.end());
            ++it;
            PPR_ASSERT(it == vec.end());
        };

        PPR_UNIT_TEST(memory_stability) {
            pP::SparseVector<int> vec;
            vec.reserve(10);

            int* ptrs[10];
            SparseKeyId keys[10];

            for(int i = 0; i < 10; ++i) {
                auto res = vec.emplace(i);
                ptrs[i] = res.getPointer();
                keys[i] = res.getKey();
            }

            vec.reserve(100);

            for(int i = 0; i < 10; ++i) {
                PPR_ASSERT(vec[keys[i]] == i);
                PPR_ASSERT(&vec[keys[i]] == ptrs[i]);
            }
        };
    }

    PPR_UNIT_TEST(sparseVector) {
        _.recurse(SparseVector::allocation_and_free_list);
        _.recurse(SparseVector::jump_counting_logic);
        _.recurse(SparseVector::key_validation);
        _.recurse(SparseVector::copy_and_move);
        _.recurse(SparseVector::iteration_boundary);
        _.recurse(SparseVector::memory_stability);
    };
}