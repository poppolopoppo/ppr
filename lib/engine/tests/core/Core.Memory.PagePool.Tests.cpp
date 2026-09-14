module;
#include "pP/UnitTest.h"

module engine.tests.core;

import engine.core;
import std;

namespace pP::tests::detail {
    namespace Page_pool {
        PPR_UNIT_TEST (bit_tree_mechanics) {
            mem::UnitTest::bit_tree_mechanics();
        };

        PPR_UNIT_TEST (bundle_flow) {
            mem::PagePool pool(4096u, 64u);

            void *ptrs[32];
            for (int i = 0; i < 32; ++i) {
                ptrs[i] = pool.allocateRaw().ptr;
            }

            for (int i = 0; i < 15; ++i) {
                pool.deallocateRaw(ptrs[i], 4096u);
            }

            pool.deallocateRaw(ptrs[15], 4096u);

            const auto a1 = pool.allocateRaw();
            PPR_TEST_ASSERT(a1.ptr != nullptr);
            PPR_TEST_ASSERT(pool.owns(a1.ptr, 4096u));
        };

        PPR_UNIT_TEST (shrink_mechanics) {
            mem::PagePool pool(4096u, 64u);

            const void *p1 = pool.allocateRaw().ptr;
            pool.deallocateRaw(p1, 4096u);

            pool.shrinkToFit();

            const void *p2 = pool.allocateRaw().ptr;
            PPR_TEST_ASSERT(p2 != nullptr);
            pool.deallocateRaw(p2, 4096u);
        };

        PPR_UNIT_TEST (allocate_deallocate_cycle) {
            mem::PagePool pool(4096u, 64u);

            void *ptrs[64];
            for (std::size_t i = 0u; i < 64u; ++i) {
                ptrs[i] = pool.allocateRaw().ptr;
                PPR_TEST_ASSERT(ptrs[i] != nullptr);
            }

            for (std::size_t i = 0u; i < 64u; ++i) {
                pool.deallocateRaw(ptrs[i], 4096u);
            }

            for (std::size_t i = 0u; i < 64u; ++i) {
                ptrs[i] = pool.allocateRaw().ptr;
                PPR_TEST_ASSERT(ptrs[i] != nullptr);
            }

            for (std::size_t i = 0u; i < 64u; ++i) {
                pool.deallocateRaw(ptrs[i], 4096u);
            }
        };
    }
} // namespace pP::tests::detail

namespace pP::tests {
    extern const UnitTest page_pool = UnitTest::Named("page_pool") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::Page_pool::bit_tree_mechanics,
            detail::Page_pool::bundle_flow,
            detail::Page_pool::shrink_mechanics,
            detail::Page_pool::allocate_deallocate_cycle,
        });
    };
} // namespace pP::tests
