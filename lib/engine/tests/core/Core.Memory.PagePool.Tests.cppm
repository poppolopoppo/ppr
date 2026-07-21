module;
#include "pP/Macros.h"
export module engine.tests.core:memory.page_pool;
import engine.core;
import std;

export namespace pP::tests {
    namespace PagePool {
        PPR_UNIT_TEST(bit_tree_mechanics) {
            mem::UnitTest::bit_tree_mechanics();
        };

        PPR_UNIT_TEST(bundle_flow) {
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
            PPR_ASSERT(a1.ptr != nullptr);
            PPR_ASSERT(pool.owns(a1.ptr, 4096u));
        };

        PPR_UNIT_TEST(shrink_mechanics) {
            mem::PagePool pool(4096u, 64u);

            const void *p1 = pool.allocateRaw().ptr;
            pool.deallocateRaw(p1, 4096u);

            pool.shrinkToFit();

            const void *p2 = pool.allocateRaw().ptr;
            PPR_ASSERT(p2 != nullptr);
            pool.deallocateRaw(p2, 4096u);
        };

        PPR_UNIT_TEST(allocate_deallocate_cycle) {
            mem::PagePool pool(4096u, 64u);

            void *ptrs[64];
            for (std::size_t i = 0u; i < 64u; ++i) {
                ptrs[i] = pool.allocateRaw().ptr;
                PPR_ASSERT(ptrs[i] != nullptr);
            }

            for (std::size_t i = 0u; i < 64u; ++i) {
                pool.deallocateRaw(ptrs[i], 4096u);
            }

            for (std::size_t i = 0u; i < 64u; ++i) {
                ptrs[i] = pool.allocateRaw().ptr;
                PPR_ASSERT(ptrs[i] != nullptr);
            }

            for (std::size_t i = 0u; i < 64u; ++i) {
                pool.deallocateRaw(ptrs[i], 4096u);
            }
        };
    }

    PPR_UNIT_TEST(pagePool) {
        _.recurse({
            PagePool::bit_tree_mechanics,
            PagePool::bundle_flow,
            PagePool::shrink_mechanics,
            PagePool::allocate_deallocate_cycle,
        });
    };
}