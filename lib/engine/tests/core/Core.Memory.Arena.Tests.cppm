module;
#include "pP/Macros.h"
export module engine.tests:core_arena;
import engine.core;
import std;

export namespace pP::tests {
    namespace Slab {
        PPR_UNIT_TEST(lifo_operations) {
            mem::InSituSlab<64u> slab;

            const auto p1 = slab.allocateRaw(16u, max_align_v);
            PPR_ASSERT(slab.owns(p1.ptr, 16u));

            bool resized = slab.resizeRaw(p1.ptr, 16u, 32u);
            PPR_ASSERT(resized);

            const auto p2 = slab.allocateRaw(16u, max_align_v);

            resized = slab.resizeRaw(p1.ptr, 32u, 64u);
            PPR_ASSERT(!resized);

            bool dealloc_res = slab.deallocateRaw(p1.ptr, 32u, max_align_v);
            PPR_ASSERT(!dealloc_res);

            dealloc_res = slab.deallocateRaw(p2.ptr, 16u, max_align_v);
            PPR_ASSERT(dealloc_res);
        };

        PPR_UNIT_TEST(out_of_memory) {
            mem::InSituSlab<64u> slab;

            const auto p1 = slab.allocateRaw(32u, max_align_v);
            PPR_ASSERT(slab.owns(p1.ptr, 32u));

            bool oom = false;
            try {
                [[maybe_unused]] const auto p2 = slab.allocateRaw(128u, max_align_v);
            } catch (std::bad_alloc) {
                oom = true;
            }
            PPR_ASSERT(oom);
            PPR_ASSERT(slab.owns(p1.ptr, 32u));
        };

        PPR_UNIT_TEST(watermark_restore) {
            mem::InSituSlab<64u> slab;
            const auto p1 = slab.allocateRaw(32u, max_align_v);

            const void *mark = slab.watermark();

            const auto p2 = slab.allocateRaw(16u, max_align_v);
            PPR_ASSERT(slab.owns(p2.ptr, p2.count));

            slab.restore(mark);

            PPR_ASSERT(slab.owns(p2.ptr, p2.count));
            PPR_ASSERT(slab.owns(p1.ptr, p1.count));

            const auto p3 = slab.allocateRaw(32u, max_align_v);
            PPR_ASSERT(slab.owns(p3.ptr, p3.count));
        };

        PPR_UNIT_TEST(move_semantics) {
            alignas(std::max_align_t) std::byte storage[64u];
            mem::Slab slab(storage);
            [[maybe_unused]] const auto p1 = slab.allocateRaw(64u, max_align_v);
            PPR_ASSERT(slab.owns(p1.ptr, p1.count));

            mem::Slab slab_moved(std::move(slab));

            PPR_ASSERT(!slab.owns(p1.ptr, p1.count));

            PPR_ASSERT(slab_moved.owns(p1.ptr, p1.count));

            slab_moved.reset();
        };

        PPR_UNIT_TEST(allocator_compliance) {
            mem::InSituSlab<512u> slab;
            mem::Allocator al = slab;

            const auto p1 = al.allocateRaw(128u);
            const auto p2 = al.allocateRaw(333u);
            PPR_ASSERT(p1.ptr != nullptr && p2.ptr != nullptr);

            al.deallocateRaw(p1.ptr, p1.count);
            al.deallocateRaw(p2.ptr, p2.count);
        };
    }

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
            [[maybe_unused]] const auto p1 = arena.allocateRaw(128u, max_align_v);
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

            const auto mark = arena.watermark();

            [[maybe_unused]] const auto p0 = arena.allocateRaw(32u, max_align_v);
            PPR_ASSERT(arena.owns(p0.ptr, 32u));

            [[maybe_unused]] auto p1 = arena.allocateRaw(16u, max_align_v);
            PPR_ASSERT(arena.owns(p1.ptr, 16u));

            bool resized = arena.resizeRaw(p1.ptr, 16u, 32u);
            PPR_ASSERT(resized);
            p1.count = 32u;

            [[maybe_unused]] const auto p2 = arena.allocateRaw(16u, max_align_v);

            resized = arena.resizeRaw(p1.ptr, 32u, 64u);
            PPR_ASSERT(!resized);

            bool dealloc_res = arena.deallocateRaw(p1.ptr, 32u, max_align_v);
            PPR_ASSERT(!dealloc_res);

            dealloc_res = arena.deallocateRaw(p2.ptr, 16u, max_align_v);
            PPR_ASSERT(dealloc_res);

            arena.restore(mark);

            PPR_ASSERT(arena.owns(p0.ptr, p0.count));
            PPR_ASSERT(arena.owns(p1.ptr, p1.count));
            PPR_ASSERT(arena.owns(p2.ptr, p2.count));
        };

        PPR_UNIT_TEST(scratch_pad_scoped) {
            auto arena = mem::ScratchPad::open();

            const auto mark = arena.watermark();

            [[maybe_unused]] const auto p0 = arena.allocateRaw(32u, max_align_v);
            PPR_ASSERT(arena.owns(p0.ptr, 32u));

            [[maybe_unused]] auto p1 = arena.allocateRaw(16u, max_align_v);
            PPR_ASSERT(arena.owns(p1.ptr, 16u));

            bool resized = arena.resizeRaw(p1.ptr, 16u, 32u);
            PPR_ASSERT(resized);
            p1.count = 32u;

            [[maybe_unused]] const auto p2 = arena.allocateRaw(16u, max_align_v);

            resized = arena.resizeRaw(p1.ptr, 32u, 64u);
            PPR_ASSERT(!resized);

            bool dealloc_res = arena.deallocateRaw(p1.ptr, 32u, max_align_v);
            PPR_ASSERT(!dealloc_res);

            dealloc_res = arena.deallocateRaw(p2.ptr, 16u, max_align_v);
            PPR_ASSERT(dealloc_res);

            arena.restore(mark);

            PPR_ASSERT(arena.owns(p0.ptr, p0.count));
            PPR_ASSERT(arena.owns(p1.ptr, p1.count));
            PPR_ASSERT(arena.owns(p2.ptr, p2.count));
        };
    }

    PPR_UNIT_TEST(slab) {
        _.recurse({
            Slab::lifo_operations,
            Slab::out_of_memory,
            Slab::watermark_restore,
            Slab::move_semantics,
            Slab::allocator_compliance,
        });
    };

    PPR_UNIT_TEST(arena) {
        _.recurse({
            Arena::lifo_operations,
            Arena::multi_slab,
            Arena::watermark_restore,
            Arena::move_semantics,
            Arena::allocator_compliance,
            Arena::scratch_pad_allocator,
            Arena::scratch_pad_scoped,
        });
    };
}
