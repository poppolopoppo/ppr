module;
#include "pP/UnitTest.h"

module engine.tests.core;

import engine.core;
import std;

namespace pP::tests::detail {
    namespace Arena {
        PPR_UNIT_TEST (lifo_operations) {
            mem::Arena<mem::GPA> arena(64u);

            const auto p1 = arena.allocateRaw(16u, max_align_v);
            PPR_TEST_ASSERT(arena.owns(p1.ptr, 16u));

            bool resized = arena.resizeRaw(p1.ptr, 16u, 32u);
            PPR_TEST_ASSERT(resized);

            const auto p2 = arena.allocateRaw(16u, max_align_v);

            resized = arena.resizeRaw(p1.ptr, 32u, 64u);
            PPR_TEST_ASSERT(!resized);

            bool dealloc_res = arena.deallocateRaw(p1.ptr, 32u, max_align_v);
            PPR_TEST_ASSERT(!dealloc_res);

            dealloc_res = arena.deallocateRaw(p2.ptr, 16u, max_align_v);
            PPR_TEST_ASSERT(dealloc_res);
        };

        PPR_UNIT_TEST (multi_slab) {
            mem::Arena<mem::GPA> arena(64u);

            const auto p1 = arena.allocateRaw(32u, max_align_v);
            PPR_TEST_ASSERT(arena.owns(p1.ptr, 32u));

            const auto p2 = arena.allocateRaw(128u, max_align_v);

            PPR_TEST_ASSERT(arena.owns(p2.ptr, 128u));
            PPR_TEST_ASSERT(arena.owns(p1.ptr, 32u));
        };

        PPR_UNIT_TEST (watermark_restore) {
            mem::Arena<mem::GPA> arena(64u);
            const auto p1 = arena.allocateRaw(32u, max_align_v);

            const void *mark = arena.watermark();

            const auto p2 = arena.allocateRaw(256u, max_align_v);
            PPR_TEST_ASSERT(arena.owns(p2.ptr, 256u));

            arena.restore(mark);

            PPR_TEST_ASSERT(!arena.owns(p2.ptr, 256u));

            PPR_TEST_ASSERT(arena.owns(p1.ptr, 32u));
        };

        PPR_UNIT_TEST (move_semantics) {
            mem::Arena<mem::GPA> arena(64u);
            [[maybe_unused]] const auto p1 = arena.allocateRaw(128u, max_align_v);
            PPR_TEST_ASSERT(arena.owns(p1.ptr, 128u));

            mem::Arena arena_moved(std::move(arena));

            PPR_TEST_ASSERT(!arena.owns(p1.ptr, 128u));

            PPR_TEST_ASSERT(arena_moved.owns(p1.ptr, 128u));

            arena_moved.reset();
        };

        PPR_UNIT_TEST (allocator_compliance) {
            mem::Arena ar(512u);
            mem::Allocator al = ar;

            void *a1 = al.allocateRaw(4096u).ptr;
            void *a2 = al.allocateRaw(4096u).ptr;
            PPR_TEST_ASSERT(a1 != nullptr && a2 != nullptr);

            al.deallocateRaw(a1, 4096u);
            al.deallocateRaw(a2, 4096u);
        };

        PPR_UNIT_TEST (scratch_pad_allocator) {
            auto arena = mem::Allocator<mem::ScratchPad>{};

            const auto mark = arena.watermark();

            [[maybe_unused]] const auto p0 = arena.allocateRaw(32u, max_align_v);
            PPR_TEST_ASSERT(arena.owns(p0.ptr, 32u));

            [[maybe_unused]] auto p1 = arena.allocateRaw(16u, max_align_v);
            PPR_TEST_ASSERT(arena.owns(p1.ptr, 16u));

            bool resized = arena.resizeRaw(p1.ptr, 16u, 32u);
            PPR_TEST_ASSERT(resized);
            p1.count = 32u;

            [[maybe_unused]] const auto p2 = arena.allocateRaw(16u, max_align_v);

            resized = arena.resizeRaw(p1.ptr, 32u, 64u);
            PPR_TEST_ASSERT(!resized);

            bool dealloc_res = arena.deallocateRaw(p1.ptr, 32u, max_align_v);
            PPR_TEST_ASSERT(!dealloc_res);

            dealloc_res = arena.deallocateRaw(p2.ptr, 16u, max_align_v);
            PPR_TEST_ASSERT(dealloc_res);

            arena.restore(mark);

            PPR_TEST_ASSERT(arena.owns(p0.ptr, p0.count));
            PPR_TEST_ASSERT(arena.owns(p1.ptr, p1.count));
            PPR_TEST_ASSERT(arena.owns(p2.ptr, p2.count));
        };

        PPR_UNIT_TEST (scratch_pad_scoped) {
            auto arena = mem::ScratchPad::open();

            const auto mark = arena.watermark();

            [[maybe_unused]] const auto p0 = arena.allocateRaw(32u, max_align_v);
            PPR_TEST_ASSERT(arena.owns(p0.ptr, 32u));

            [[maybe_unused]] auto p1 = arena.allocateRaw(16u, max_align_v);
            PPR_TEST_ASSERT(arena.owns(p1.ptr, 16u));

            bool resized = arena.resizeRaw(p1.ptr, 16u, 32u);
            PPR_TEST_ASSERT(resized);
            p1.count = 32u;

            [[maybe_unused]] const auto p2 = arena.allocateRaw(16u, max_align_v);

            resized = arena.resizeRaw(p1.ptr, 32u, 64u);
            PPR_TEST_ASSERT(!resized);

            bool dealloc_res = arena.deallocateRaw(p1.ptr, 32u, max_align_v);
            PPR_TEST_ASSERT(!dealloc_res);

            dealloc_res = arena.deallocateRaw(p2.ptr, 16u, max_align_v);
            PPR_TEST_ASSERT(dealloc_res);

            arena.restore(mark);

            PPR_TEST_ASSERT(arena.owns(p0.ptr, p0.count));
            PPR_TEST_ASSERT(arena.owns(p1.ptr, p1.count));
            PPR_TEST_ASSERT(arena.owns(p2.ptr, p2.count));
        };
    }
} // namespace pP::tests::detail

namespace pP::tests {
    extern const UnitTest arena = UnitTest::Named("arena") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::Arena::lifo_operations,
            detail::Arena::multi_slab,
            detail::Arena::watermark_restore,
            detail::Arena::move_semantics,
            detail::Arena::allocator_compliance,
            detail::Arena::scratch_pad_allocator,
            detail::Arena::scratch_pad_scoped,
        });
    };
} // namespace pP::tests
