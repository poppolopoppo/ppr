module;
#include "pP/Macros.h"
export module engine.tests:core_arena;
import engine.core;
import std;

export namespace pP::tests {
    namespace Arena {
        PPR_UNIT_TEST(lifo_operations) {
            mem::Arena<mem::GPA> arena(64u);

            const auto p1 = arena.allocateRaw(16u, max_align_v);
            PPR_ASSERT(arena.owns(p1.ptr, 16u));

            bool resized = arena.resizeRaw(p1.ptr, 16u, 32u);
            PPR_ASSERT(resized);

            const auto p2 = arena.allocateRaw(16u, max_align_v);

            resized = arena.resizeRaw(p1.ptr, 32u, 64u);
            PPR_ASSERT(!resized);

            bool dealloc_res = arena.deallocateRaw(p1.ptr, 32u, max_align_v);
            PPR_ASSERT(!dealloc_res);

            dealloc_res = arena.deallocateRaw(p2.ptr, 16u, max_align_v);
            PPR_ASSERT(dealloc_res);
        };

        PPR_UNIT_TEST(multi_slab) {
            mem::Arena<mem::GPA> arena(64u);

            const auto p1 = arena.allocateRaw(32u, max_align_v);
            PPR_ASSERT(arena.owns(p1.ptr, 32u));

            const auto p2 = arena.allocateRaw(128u, max_align_v);

            PPR_ASSERT(arena.owns(p2.ptr, 128u));
            PPR_ASSERT(arena.owns(p1.ptr, 32u));
        };

        PPR_UNIT_TEST(watermark_restore) {
            mem::Arena<mem::GPA> arena(64u);
            const auto p1 = arena.allocateRaw(32u, max_align_v);

            const void *mark = arena.watermark();

            const auto p2 = arena.allocateRaw(256u, max_align_v);
            PPR_ASSERT(arena.owns(p2.ptr, 256u));

            arena.restore(mark);

            PPR_ASSERT(!arena.owns(p2.ptr, 256u));

            PPR_ASSERT(arena.owns(p1.ptr, 32u));
        };

        PPR_UNIT_TEST(move_semantics) {
            mem::Arena<mem::GPA> arena(64u);
            const auto p1 = arena.allocateRaw(128u, max_align_v);
            PPR_ASSERT(arena.owns(p1.ptr, 128u));

            mem::Arena arena_moved(std::move(arena));

            PPR_ASSERT(!arena.owns(p1.ptr, 128u));

            PPR_ASSERT(arena_moved.owns(p1.ptr, 128u));

            arena_moved.reset();
        };

        PPR_UNIT_TEST(allocator_compliance) {
            mem::Arena ar(512u);
            mem::Allocator al = ar;

            void *a1 = al.allocateRaw(4096u).ptr;
            void *a2 = al.allocateRaw(4096u).ptr;
            PPR_ASSERT(a1 != nullptr && a2 != nullptr);

            al.deallocateRaw(a1, 4096u);
            al.deallocateRaw(a2, 4096u);
        };

        PPR_UNIT_TEST(scratch_pad_allocator) {
            auto arena = mem::Allocator<mem::ScratchPad>{};

            const auto mark0 = arena.watermark();

            const auto p0 = arena.allocateRaw(32u);
            PPR_ASSERT(arena.owns(p0.ptr, 32u));

            const auto mark1 = arena.watermark();

            const auto p1 = arena.allocateRaw(16u, max_align_v);
            PPR_ASSERT(arena.owns(p1.ptr, 16u));

            bool resized = arena.resizeRaw(p1.ptr, 16u, 32u);
            PPR_ASSERT(resized);

            const auto p2 = arena.allocateRaw(16u, max_align_v);

            resized = arena.resizeRaw(p1.ptr, 32u, 64u);
            PPR_ASSERT(!resized);

            bool dealloc_res = arena.deallocateRaw(p1.ptr, 32u, max_align_v);
            PPR_ASSERT(!dealloc_res);

            dealloc_res = arena.deallocateRaw(p2.ptr, 16u, max_align_v);
            PPR_ASSERT(dealloc_res);

            arena.restore(mark1);

            PPR_ASSERT(arena.owns(p0.ptr, 32u));
            PPR_ASSERT(!arena.owns(p1.ptr, 16u));
            PPR_ASSERT(!arena.owns(p2.ptr, 16u));

            arena.restore(mark0);

            PPR_ASSERT(!arena.owns(p0.ptr, 32u));
            PPR_ASSERT(!arena.owns(p1.ptr, 16u));
            PPR_ASSERT(!arena.owns(p2.ptr, 16u));
        };

        PPR_UNIT_TEST(scratch_pad_scoped) {
            auto arena = mem::ScratchPad::open();

            const auto mark0 = arena.watermark();

            const auto p0 = arena.allocateRaw(32u, max_align_v);
            PPR_ASSERT(arena.owns(p0.ptr, 32u));

            const auto mark1 = arena.watermark();

            const auto p1 = arena.allocateRaw(16u, max_align_v);
            PPR_ASSERT(arena.owns(p1.ptr, 16u));

            bool resized = arena.resizeRaw(p1.ptr, 16u, 32u);
            PPR_ASSERT(resized);

            const auto p2 = arena.allocateRaw(16u, max_align_v);

            resized = arena.resizeRaw(p1.ptr, 32u, 64u);
            PPR_ASSERT(!resized);

            bool dealloc_res = arena.deallocateRaw(p1.ptr, 32u, max_align_v);
            PPR_ASSERT(!dealloc_res);

            dealloc_res = arena.deallocateRaw(p2.ptr, 16u, max_align_v);
            PPR_ASSERT(dealloc_res);

            arena.restore(mark1);

            PPR_ASSERT(arena.owns(p0.ptr, 32u));
            PPR_ASSERT(!arena.owns(p1.ptr, 16u));
            PPR_ASSERT(!arena.owns(p2.ptr, 16u));

            arena.restore(mark0);

            PPR_ASSERT(!arena.owns(p0.ptr, 32u));
            PPR_ASSERT(!arena.owns(p1.ptr, 16u));
            PPR_ASSERT(!arena.owns(p2.ptr, 16u));
        };
    }

    PPR_UNIT_TEST(arena) {
        _.recurse(Arena::lifo_operations);
        _.recurse(Arena::multi_slab);
        _.recurse(Arena::watermark_restore);
        _.recurse(Arena::move_semantics);
        _.recurse(Arena::allocator_compliance);
        _.recurse(Arena::scratch_pad_allocator);
        _.recurse(Arena::scratch_pad_scoped);
    };
}