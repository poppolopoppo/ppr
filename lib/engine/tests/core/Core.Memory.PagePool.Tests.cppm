module;
#include "pP/Macros.h"
export module engine.tests:core_page_pool;
import engine.core;
import std;

export namespace pP::tests {
    namespace PagePool {
        PPR_UNIT_TEST(bit_tree_mechanics) {
            mem::BitmapTree tree;
            const mem::BitmapTree::BuildInfos infos(128u);

            std::byte storage[1024];
            tree.initialize(infos, {storage, infos.getAllocationSize()}, false);
            PPR_ASSERT(!tree.isFull());

            bool was_empty = false;
            const u32 b1 = tree.allocate(infos, was_empty);

            PPR_ASSERT(b1 == 0u);
            PPR_ASSERT(was_empty);
            PPR_ASSERT(tree.isAllocated(infos, 0u));

            const auto range = tree.allocateContiguous(infos, 10u, was_empty);
            PPR_ASSERT(range.m_first_bit == 1u);
            PPR_ASSERT(range.m_bit_count == 10u);
            PPR_ASSERT(!was_empty);

            tree.deallocate(infos, 0u);
            PPR_ASSERT(!tree.isAllocated(infos, 0u));

            const u32 next = tree.nextAllocateBit(infos, 0u);
            PPR_ASSERT(next == 0u);
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

        PPR_UNIT_TEST(hint_cache_behavior) {
            mem::PagePool pool(4096u, 64u);
            mem::PagePool::Hint<2u> hint(pool);

            void *h1 = hint.allocateRaw().ptr;
            void *h2 = hint.allocateRaw().ptr;
            void *h3 = hint.allocateRaw().ptr;

            hint.deallocateRaw(h1, 4096u);
            hint.deallocateRaw(h2, 4096u);

            hint.deallocateRaw(h3, 4096u);

            const auto r1 = hint.allocateRaw();
            const auto r2 = hint.allocateRaw();
            const auto r3 = hint.allocateRaw();

            PPR_ASSERT(r1.ptr == h3);
            PPR_ASSERT(r2.ptr == h2);
            PPR_ASSERT(r3.ptr == h1);

            hint.shrinkToFit(true);
        };
    }

    PPR_UNIT_TEST(pagePool) {
        _.recurse(PagePool::bit_tree_mechanics);
        _.recurse(PagePool::bundle_flow);
        _.recurse(PagePool::shrink_mechanics);
        _.recurse(PagePool::hint_cache_behavior);
    };
}