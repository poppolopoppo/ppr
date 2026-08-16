module;
#include "pP/UnitTest.h"
export module engine.tests.core:containers.flat_map;

import std;
import engine.core;

export namespace pP::tests {
    namespace FlatMap {
        PPR_UNIT_TEST(empty) {
            pP::FlatMap<int, int> m;
            PPR_TEST_ASSERT(m.empty());
            PPR_TEST_ASSERT(m.size() == 0u);
            PPR_TEST_ASSERT(m.find(42) == m.end());
        };

        PPR_UNIT_TEST(single_insert) {
            pP::FlatMap<int, int> m;
            auto [it, inserted] = m.insert({1, 10});
            PPR_TEST_ASSERT(inserted);
            PPR_TEST_ASSERT(it->first == 1);
            PPR_TEST_ASSERT(it->second == 10);
            PPR_TEST_ASSERT(m.size() == 1u);
            PPR_TEST_ASSERT(!m.empty());
        };

        PPR_UNIT_TEST(find) {
            pP::FlatMap<int, int> m;
            m.insert({1, 10});
            m.insert({3, 30});
            m.insert({2, 20});
            auto it = m.find(2);
            PPR_TEST_ASSERT(it != m.end());
            PPR_TEST_ASSERT(it->second == 20);
            PPR_TEST_ASSERT(m.find(4) == m.end());
        };

        PPR_UNIT_TEST(duplicate) {
            pP::FlatMap<int, int> m;
            m.insert({1, 10});
            auto [it, inserted] = m.insert({1, 99});
            PPR_TEST_ASSERT(!inserted);
            PPR_TEST_ASSERT(it->second == 10);
            PPR_TEST_ASSERT(m.size() == 1u);
        };

        PPR_UNIT_TEST(erase) {
            pP::FlatMap<int, int> m;
            m.insert({1, 10});
            m.insert({2, 20});
            m.insert({3, 30});
            PPR_TEST_ASSERT(m.erase(2));
            PPR_TEST_ASSERT(m.size() == 2u);
            PPR_TEST_ASSERT(m.find(2) == m.end());
            PPR_TEST_ASSERT(m.find(1) != m.end());
            PPR_TEST_ASSERT(m.find(3) != m.end());
            PPR_TEST_ASSERT(m.erase(99) == 0u);
        };

        PPR_UNIT_TEST(clear) {
            pP::FlatMap<int, int> m;
            m.insert({1, 10});
            m.insert({2, 20});
            m.clear();
            PPR_TEST_ASSERT(m.empty());
            PPR_TEST_ASSERT(m.find(1) == m.end());
            PPR_TEST_ASSERT(m.find(2) == m.end());
        };

        PPR_UNIT_TEST(iteration_sorted) {
            pP::FlatMap<int, int> m;
            m.insert({4, 40});
            m.insert({2, 20});
            m.insert({6, 60});
            m.insert({1, 10});
            m.insert({3, 30});
            m.insert({5, 50});
            m.insert({7, 70});
            pP::Array<int> keys;
            for (auto [k, v] : m) {
                keys.push_back(k);
            }
            PPR_TEST_ASSERT(keys.size() == 7u);
            PPR_TEST_ASSERT(keys[0] == 1);
            PPR_TEST_ASSERT(keys[1] == 2);
            PPR_TEST_ASSERT(keys[2] == 3);
            PPR_TEST_ASSERT(keys[3] == 4);
            PPR_TEST_ASSERT(keys[4] == 5);
            PPR_TEST_ASSERT(keys[5] == 6);
            PPR_TEST_ASSERT(keys[6] == 7);
        };

        PPR_UNIT_TEST(lower_bound) {
            pP::FlatMap<int, int> m;
            m.insert({1, 10});
            m.insert({3, 30});
            m.insert({5, 50});
            auto it = m.lower_bound(2);
            PPR_TEST_ASSERT(it->first == 3);
        };

        PPR_UNIT_TEST(upper_bound) {
            pP::FlatMap<int, int> m;
            m.insert({1, 10});
            m.insert({3, 30});
            m.insert({5, 50});
            auto it = m.upper_bound(3);
            PPR_TEST_ASSERT(it->first == 5);
        };

        PPR_UNIT_TEST(contains) {
            pP::FlatMap<int, int> m;
            m.insert({1, 10});
            PPR_TEST_ASSERT(m.contains(1));
            PPR_TEST_ASSERT(!m.contains(2));
        };

        PPR_UNIT_TEST(at) {
            pP::FlatMap<int, int> m;
            m.insert({1, 10});
            PPR_TEST_ASSERT(m.at(1) == 10);
        };

        PPR_UNIT_TEST(operator_sq) {
            pP::FlatMap<int, int> m;
            m.insert({1, 10});
            PPR_TEST_ASSERT(m[1] == 10);
        };

        PPR_UNIT_TEST(const_find) {
            const pP::FlatMap<int, int> m{{1, 10}, {2, 20}};
            const auto it = m.find(1);
            PPR_TEST_ASSERT(m.end() != it);
            PPR_TEST_ASSERT(it->second == 10);
        };

        PPR_UNIT_TEST(const_at) {
            const pP::FlatMap<int, int> m{{1, 10}};
            PPR_TEST_ASSERT(m.at(1) == 10);
        };

        PPR_UNIT_TEST(const_contains) {
            const pP::FlatMap<int, int> m{{1, 10}};
            PPR_TEST_ASSERT(m.contains(1));
            PPR_TEST_ASSERT(!m.contains(2));
        };

        PPR_UNIT_TEST(copy) {
            pP::FlatMap<int, int> a;
            a.insert({1, 10});
            a.insert({2, 20});
            pP::FlatMap<int, int> b = a;
            PPR_TEST_ASSERT(b.size() == a.size());
            PPR_TEST_ASSERT(b.find(1)->second == 10);
            PPR_TEST_ASSERT(b.find(2)->second == 20);
        };

        PPR_UNIT_TEST(move) {
            pP::FlatMap<int, int> a;
            a.insert({1, 10});
            pP::FlatMap<int, int> b = std::move(a);
            PPR_TEST_ASSERT(b.find(1) != b.end());
            PPR_TEST_ASSERT(a.empty());
        };

        PPR_UNIT_TEST(growth) {
            pP::FlatMap<int, int> m;
            for (int i = 0; i < 100; ++i) {
                m.insert({i, i});
            }
            PPR_TEST_ASSERT(m.size() == 100u);
            for (int i = 0; i < 100; ++i) {
                PPR_TEST_ASSERT(m.contains(i));
            }
        };

        PPR_UNIT_TEST(range_insert) {
            pP::FlatMap<int, int> m;
            m.insert({1, 10});
            m.insert({3, 30});

            pP::Array<std::pair<int, int>> more{{2, 20}, {4, 40}, {5, 50}};
            m.insert(more.begin(), more.end());

            PPR_TEST_ASSERT(m.size() == 5u);
            PPR_TEST_ASSERT(m.find(1)->second == 10);
            PPR_TEST_ASSERT(m.find(2)->second == 20);
            PPR_TEST_ASSERT(m.find(3)->second == 30);
            PPR_TEST_ASSERT(m.find(4)->second == 40);
            PPR_TEST_ASSERT(m.find(5)->second == 50);
        };

        PPR_UNIT_TEST(range_insert_duplicate) {
            pP::FlatMap<int, int> m;
            m.insert({1, 10});
            m.insert({2, 20});

            pP::Array<std::pair<int, int>> dups{{2, 99}, {3, 30}};
            m.insert(dups.begin(), dups.end());

            PPR_TEST_ASSERT(m.size() == 3u);
            PPR_TEST_ASSERT(m.find(2)->second == 20);
            PPR_TEST_ASSERT(m.find(3)->second == 30);
        };

        PPR_UNIT_TEST(range_insert_empty) {
            pP::FlatMap<int, int> m;
            m.insert({1, 10});
            pP::Array<std::pair<int, int>> no_entries;
            m.insert(no_entries.begin(), no_entries.end());
            PPR_TEST_ASSERT(m.size() == 1u);
            PPR_TEST_ASSERT(m.find(1)->second == 10);
        };

        PPR_UNIT_TEST(initializer_list) {
            pP::FlatMap<int, int> m{{2, 20}, {1, 10}};
            PPR_TEST_ASSERT(m.size() == 2u);
            PPR_TEST_ASSERT(m.find(1)->second == 10);
            PPR_TEST_ASSERT(m.find(2)->second == 20);
        };
    }

    PPR_UNIT_TEST(flatMap) {
        _.recurse({
            FlatMap::empty,
            FlatMap::single_insert,
            FlatMap::find,
            FlatMap::duplicate,
            FlatMap::erase,
            FlatMap::clear,
            FlatMap::iteration_sorted,
            FlatMap::lower_bound,
            FlatMap::upper_bound,
            FlatMap::contains,
            FlatMap::at,
            FlatMap::operator_sq,
            FlatMap::const_find,
            FlatMap::const_at,
            FlatMap::const_contains,
            FlatMap::copy,
            FlatMap::move,
            FlatMap::growth,
            FlatMap::range_insert,
            FlatMap::range_insert_duplicate,
            FlatMap::range_insert_empty,
            FlatMap::initializer_list,
        });
    };
}
