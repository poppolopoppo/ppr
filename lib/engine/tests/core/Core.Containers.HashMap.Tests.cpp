module;
#include "pP/UnitTest.h"

module engine.tests.core;

import engine.core;
import std;

namespace pP::tests::detail {
    namespace Hash_map {
        PPR_UNIT_TEST (eviction) {
            pP::HashMap<int, int> m;
            m.insert({0, 0});
            m.insert({2, 2});
            PPR_TEST_ASSERT(m.find(0) != m.end());
            PPR_TEST_ASSERT(m.find(2) != m.end());
        };

        PPR_UNIT_TEST (const_find) {
            const pP::HashMap<int, int> m{{1, 10}, {2, 20}};
            const auto it = m.find(1);
            PPR_TEST_ASSERT(m.end() != it);
        };

        PPR_UNIT_TEST (begin_empty_allocated) {
            pP::HashMap<int, int> m{{1, 10}};
            int count = 0;
            int sum = 0;
            for (auto [k,v]: m) {
                count += k;
                sum += v;
            }
            PPR_TEST_ASSERT(count == 1);
            PPR_TEST_ASSERT(sum == 10);
            m.clear();
            count = 0;
            sum = 0;
            for (auto [k,v]: m) {
                count += k;
                sum += v;
            }
            PPR_TEST_ASSERT(count == 0);
            PPR_TEST_ASSERT(sum == 0);
        };

        PPR_UNIT_TEST (erase) {
            pP::HashMap<int, int> m{{1, 10}, {2, 20}, {3, 30}};
            m.erase(2);
            PPR_TEST_ASSERT(m.find(2) == m.end());
            PPR_TEST_ASSERT(m.find(1) != m.end());
            PPR_TEST_ASSERT(m.find(3) != m.end());
        };

        PPR_UNIT_TEST (move) {
            pP::HashMap<int, int> a{{1, 10}};
            pP::HashMap b(std::move(a));
            PPR_TEST_ASSERT(b.find(1) != b.end());
            PPR_TEST_ASSERT(a.size() == 0);
        };

        PPR_UNIT_TEST (duplicate_size) {
            pP::HashMap<int, int> m;
            m.insert({1, 10});
            auto [it, inserted] = m.insert({1, 99});
            PPR_TEST_ASSERT(!inserted);
            PPR_TEST_ASSERT(m.size() == 1);
            PPR_TEST_ASSERT(it->second == 10);
        };

        PPR_UNIT_TEST (find_empty) {
            pP::HashMap<int, int> m;
            PPR_TEST_ASSERT(m.find(42) == m.end());
        };

        PPR_UNIT_TEST (find_after_eviction) {
            pP::HashMap<int, int> m(4u);
            for (int i = 0; i < 3; ++i) {
                m.insert({i * 4, i});
            }
            for (int i = 0; i < 3; ++i) {
                PPR_TEST_ASSERT(m.find(i * 4) != m.end());
            }
        };

        PPR_UNIT_TEST (unordered_equality) {
            const pP::HashMap<int, int> a = {{2, 3}, {3, 4}, {4, 5}};
            pP::HashMap<int, int> b = a;
            PPR_TEST_ASSERT(a == b);
            b.insert({6, 7});
            PPR_TEST_ASSERT(a != b);
        };

        PPR_UNIT_TEST (unordered_hash_value) {
            const HashSet<int> a = {2, 3, 4, 5};
            HashSet<int> b = a;
            const hash_t ha = hashValue(a);
            const hash_t hb = hashValue(b);
            PPR_TEST_ASSERT(ha == hb);
            b.append({6, 7});
            const hash_t hc = hashValue(b);
            PPR_TEST_ASSERT(hb != hc);
        };

        PPR_UNIT_TEST(stale_iterator_after_reserve_crashes, UnitTest::expect_crash) {
            pP::HashMap<int, int> m{{1, 10}};
            auto it = m.find(1);
            PPR_TEST_ASSERT(it != m.end());
            m.reserve(64u);
            [[maybe_unused]] auto val = *it;
        };
    }
} // namespace pP::tests::detail

namespace pP::tests {
    extern const UnitTest hash_map = UnitTest::Named("hash_map") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::Hash_map::eviction,
            detail::Hash_map::const_find,
            detail::Hash_map::begin_empty_allocated,
            detail::Hash_map::erase,
            detail::Hash_map::move,
            detail::Hash_map::duplicate_size,
            detail::Hash_map::find_empty,
            detail::Hash_map::find_after_eviction,
            detail::Hash_map::unordered_equality,
            detail::Hash_map::unordered_hash_value,
        });
        if constexpr (PPR_ENABLE_DEBUG) {
            _.recurse(detail::Hash_map::stale_iterator_after_reserve_crashes);
        }
    };
} // namespace pP::tests
