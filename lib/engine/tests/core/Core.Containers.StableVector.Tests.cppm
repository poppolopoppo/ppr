module;
#include "pP/Macros.h"
export module engine.tests:core_stable_vector;
import engine.core;
import std;

export namespace pP::tests {
    namespace StableVector {
        PPR_UNIT_TEST(growth_and_indexing) {
            pP::StableVector<int> vec;

            PPR_ASSERT(vec.isEmpty());
            PPR_ASSERT(vec.size() == 0);

            for (std::size_t i = 0; i < 20; ++i) {
                vec.pushBack(static_cast<int>(i));
            }

            for (std::size_t i = 0; i < 20; ++i) {
                PPR_ASSERT(vec[i] == static_cast<int>(i));
            }

            vec.reserve(100);
            PPR_ASSERT(vec.capacity() >= 100);
            PPR_ASSERT(vec.size() == 20);
        };

        PPR_UNIT_TEST(iterator_navigation) {
            pP::StableVector<int> vec;
            for (std::size_t i = 0; i < 64; ++i) {
                vec.pushBack(static_cast<int>(i));
            }

            auto it = vec.begin();

            it += 50;
            PPR_ASSERT(*it == 50);
            PPR_ASSERT(it.getIndex() == 50);

            PPR_ASSERT(vec.end() - vec.begin() == 64);

            auto back_it = vec.end();
            --back_it;
            PPR_ASSERT(*back_it == 63);

            PPR_ASSERT(vec.begin() + 10 == vec.begin() + 10);
            PPR_ASSERT(vec.begin() + vec.size() == vec.end());
        };

        struct Mock {
            static inline int count = 0;
            Mock() { count++; }
            ~Mock() { count--; }
            Mock(const Mock &) { count++; }
        };

        PPR_UNIT_TEST(lifetime_management) {
            {
                pP::StableVector<Mock> vec;
                vec.resize(10);
                PPR_ASSERT(Mock::count == 10);

                vec.clear();
                PPR_ASSERT(Mock::count == 0);
                PPR_ASSERT(vec.size() == 0);

                vec.resize(5);
                PPR_ASSERT(Mock::count == 5);
            }
            PPR_ASSERT(Mock::count == 0);
        };

        PPR_UNIT_TEST(modifiers) {
            pP::StableVector<int> vec = {0, 1, 2, 3, 4};

            vec.erase(2);
            PPR_ASSERT(vec.size() == 4);
            PPR_ASSERT(vec[2] == 3);

            vec.eraseSwapBack(0);
            PPR_ASSERT(vec[0] == 4);

            pP::StableVector<int> other;
            other.pushBack(99);
            vec = std::move(other);
            PPR_ASSERT(vec.size() == 1);
            PPR_ASSERT(vec[0] == 99);
            PPR_ASSERT(other.isEmpty());
        };

        PPR_UNIT_TEST(memory_compaction) {
            pP::StableVector<int> vec;
            vec.reserve(10);
            for (std::size_t i = 0; i < 10; ++i) {
                vec.pushBack(static_cast<int>(i));
            }
            vec.reserve(128);

            vec.shrinkToFit();
            PPR_ASSERT(vec.capacity() >= 10);
            PPR_ASSERT(vec.capacity() < 128);

            for (std::size_t i = 0; i < 10; ++i) {
                PPR_ASSERT(vec[i] == static_cast<int>(i));
            }
        };
    }

    PPR_UNIT_TEST(stableVector) {
        _.recurse(StableVector::growth_and_indexing);
        _.recurse(StableVector::iterator_navigation);
        _.recurse(StableVector::lifetime_management);
        _.recurse(StableVector::modifiers);
        _.recurse(StableVector::memory_compaction);
    };
}