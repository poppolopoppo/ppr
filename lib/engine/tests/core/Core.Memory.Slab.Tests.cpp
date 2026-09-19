module;
#include "pP/UnitTest.h"

module engine.tests.core;

import engine.core;
import std;

namespace pP::tests::detail {
    namespace Slab {
        PPR_UNIT_TEST (lifo_operations) {
            mem::InSituSlab<64u> slab;

            const auto p1 = slab.allocateRaw(16u, max_align_v);
            PPR_TEST_ASSERT(slab.owns(p1.ptr, 16u));

            bool resized = slab.resizeRaw(p1.ptr, 16u, 32u);
            PPR_TEST_ASSERT(resized);

            const auto p2 = slab.allocateRaw(16u, max_align_v);

            resized = slab.resizeRaw(p1.ptr, 32u, 64u);
            PPR_TEST_ASSERT(!resized);

            bool dealloc_res = slab.deallocateRaw(p1.ptr, 32u, max_align_v);
            PPR_TEST_ASSERT(!dealloc_res);

            dealloc_res = slab.deallocateRaw(p2.ptr, 16u, max_align_v);
            PPR_TEST_ASSERT(dealloc_res);
        };

        PPR_UNIT_TEST (out_of_memory) {
            mem::InSituSlab<64u> slab;

            const auto p1 = slab.allocateRaw(32u, max_align_v);
            PPR_TEST_ASSERT(slab.owns(p1.ptr, 32u));

            bool oom = false;
            try {
                [[maybe_unused]] const auto p2 = slab.allocateRaw(128u, max_align_v);
            } catch (std::bad_alloc) {
                oom = true;
            }
            PPR_TEST_ASSERT(oom);
            PPR_TEST_ASSERT(slab.owns(p1.ptr, 32u));
        };

        PPR_UNIT_TEST (watermark_restore) {
            mem::InSituSlab<64u> slab;
            const auto p1 = slab.allocateRaw(32u, max_align_v);

            const void *mark = slab.watermark();

            const auto p2 = slab.allocateRaw(16u, max_align_v);
            PPR_TEST_ASSERT(slab.owns(p2.ptr, p2.count));

            slab.restore(mark);

            PPR_TEST_ASSERT(slab.owns(p2.ptr, p2.count));
            PPR_TEST_ASSERT(slab.owns(p1.ptr, p1.count));

            const auto p3 = slab.allocateRaw(32u, max_align_v);
            PPR_TEST_ASSERT(slab.owns(p3.ptr, p3.count));
        };

        PPR_UNIT_TEST (move_semantics) {
            alignas(std::max_align_t) std::byte storage[64u];
            mem::Slab slab(storage);
            [[maybe_unused]] const auto p1 = slab.allocateRaw(64u, max_align_v);
            PPR_TEST_ASSERT(slab.owns(p1.ptr, p1.count));

            mem::Slab slab_moved(std::move(slab));

            PPR_TEST_ASSERT(!slab.owns(p1.ptr, p1.count));

            PPR_TEST_ASSERT(slab_moved.owns(p1.ptr, p1.count));

            slab_moved.reset();
        };

        PPR_UNIT_TEST (allocator_compliance) {
            mem::InSituSlab<512u> slab;
            mem::Allocator al = slab;

            const auto p1 = al.allocateRaw(128u);
            const auto p2 = al.allocateRaw(333u);
            PPR_TEST_ASSERT(p1.ptr != nullptr && p2.ptr != nullptr);

            al.deallocateRaw(p1.ptr, p1.count);
            al.deallocateRaw(p2.ptr, p2.count);
        };
    }
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest slab = UnitTest::Named("slab") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::Slab::lifo_operations,
            detail::Slab::out_of_memory,
            detail::Slab::watermark_restore,
            detail::Slab::move_semantics,
            detail::Slab::allocator_compliance,
        });
    };

    const UnitTest &slabTests() noexcept {
        return slab;
    }
} // namespace pP::tests
