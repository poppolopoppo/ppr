module;
#include "pP/Macros.h"
export module engine.tests:core_containers;
import engine.core;
import std;

export namespace pP::tests {
    namespace Containers {
        PPR_UNIT_TEST(traits_relocatable_fundamentals) {
            PPR_ASSERT(details::relocatable<int>::value);
            PPR_ASSERT(details::relocatable<float>::value);
            PPR_ASSERT(details::relocatable<double>::value);
            PPR_ASSERT(details::relocatable<void *>::value);
            PPR_ASSERT(details::relocatable<int *>::value);
            PPR_ASSERT(details::relocatable<int[10]>::value);
        };

        PPR_UNIT_TEST(traits_relocatable_user_type_negative) {
            struct NonRelocatable {
                NonRelocatable() = default;
                NonRelocatable(const NonRelocatable &) {
                }
            };

            struct TriviallyCopyableNonPrimitive {
                int a;
                float b;
            };

            PPR_ASSERT(!details::relocatable<NonRelocatable>::value);
            PPR_ASSERT(!details::relocatable<TriviallyCopyableNonPrimitive>::value);
        };

        PPR_UNIT_TEST(traits_relocatable_specialized_types) {
            PPR_ASSERT(details::is_relocatable_v<Bitmask<std::uint64_t>>);
            PPR_ASSERT(details::is_relocatable_v<Stack<float, 15>>);
            PPR_ASSERT(details::is_relocatable_v<RingBuffer<int, 8>>);
        };

        PPR_UNIT_TEST(bitmask_basic_set_reset_test) {
            Bitmask<> m;
            PPR_ASSERT(m.none());
            m.set(0);
            PPR_ASSERT(m.test(0));
            m.reset(0);
            PPR_ASSERT(!m.test(0));
        };

        PPR_UNIT_TEST(bitmask_range_and_flip_test) {
            Bitmask<> m;
            m.setRange(4, 4);
            PPR_ASSERT(m.countOnes() == 4u);
            m.unsetRange(4, 4);
            PPR_ASSERT(m.none());
            m.flip(7);
            PPR_ASSERT(m.test(7));
            m.flip(7);
            PPR_ASSERT(!m.test(7));
        };

        PPR_UNIT_TEST(bitmask_rotate_and_pop_tests) {
            Bitmask<std::uint32_t> m32;
            m32.set(0);
            m32.rotateLeft(1);
            PPR_ASSERT(m32.test(1));
            m32.rotateRight(1);
            PPR_ASSERT(m32.test(0));

            Bitmask<> m;
            m.unsetAll();
            m.set(2);
            m.set(5);
            const u32 first = m.pop();
            PPR_ASSERT(first == 2u);
            m.set(1);
            PPR_ASSERT(m.popAssumeNotEmpty() != umax_v);
        };

        PPR_UNIT_TEST(bitmask_byteSwap_and_invert_and_setFirstLastUnsetFirst) {
            Bitmask<std::uint32_t> m;
            m.set(0);
            auto swapped = m.byteSwap();
            PPR_ASSERT(std::is_same_v<decltype(swapped), Bitmask<std::uint32_t>>);

            auto inv = m.invert();
            PPR_ASSERT(inv.any());

            auto first3 = Bitmask<std::uint32_t>::setFirstN(3);
            PPR_ASSERT(first3.countOnes() == 3u);
            auto last2 = Bitmask<std::uint32_t>::setLastN(2);
            PPR_ASSERT(last2.countOnes() == 2u);
            auto unsetFirst1 = Bitmask<std::uint32_t>::unsetFirstN(1);
            PPR_ASSERT(unsetFirst1.countOnes() == 31u);
        };

        PPR_UNIT_TEST(bitmask_ref_mutation_visibility) {
            std::uint64_t storage = 0;
            BitmaskRef<std::uint64_t> ref(storage);
            ref.set(0);
            PPR_ASSERT((storage & 1ULL) != 0ULL);
            ref.unsetAll();
            PPR_ASSERT(storage == 0ULL);
        };

        PPR_UNIT_TEST(bitmask_ref_compound_ops) {
            std::uint64_t storage = 0;
            BitmaskRef<std::uint64_t> a(storage), b(storage);
            a.set(3);
            b.set(5);
            a |= b;
            PPR_ASSERT(a.test(3) && a.test(5));
            a -= b;
            PPR_ASSERT(!a.test(5));
            a ^= b;
            PPR_ASSERT((a.cref() & b.cref()) == 0u);
        };

        PPR_UNIT_TEST(relptr_null_and_valid) {
            RelPtr<int> p(nullptr);
            PPR_ASSERT(!p);
            int x = 42;
            RelPtr q(&x);
            PPR_ASSERT(q.isValid());
            PPR_ASSERT(*q == 42);
        };

        PPR_UNIT_TEST(relptr_copy_assign_and_comparisons) {
            int x = 7;
            RelPtr a(&x);
            RelPtr<int> b = a;
            PPR_ASSERT(b.isValid());
            RelPtr<int> c;
            c = a;
            PPR_ASSERT(c.getData() == a.getData());
            PPR_ASSERT(a == b);
            PPR_ASSERT((a <=> b) == std::strong_ordering::equal);
            PPR_ASSERT(a == &x);
        };

        PPR_UNIT_TEST(tagptr_basic_tag_and_data) {
            alignas(16) int x = 7;
            TagPtr<int, std::uintptr_t, static_cast<std::align_val_t>(16)> t(&x, 3u);
            PPR_ASSERT(t.getData() == &x);
            PPR_ASSERT(t.getTag() == 3u);
            PPR_ASSERT(t.hasTag(static_cast<std::uintptr_t>(3u)));
        };

        PPR_UNIT_TEST(tagptr_bits_reinterpret_and_mutation) {
            alignas(16) int x = 9;
            TagPtr<int, std::uintptr_t, static_cast<std::align_val_t>(16)> t(&x, 1u);
            auto bits = t.getBits();
            PPR_ASSERT(bits.any() == (t.getTag() != 0));
            PPR_ASSERT(t.getReinterpret<int>() == &x);
            t.setTag(0u);
            PPR_ASSERT(t.getTag() == 0u);
            t.setData(&x);
            PPR_ASSERT(t.getData() == &x);

            TagPtr<int, std::uintptr_t, static_cast<std::align_val_t>(16)> t2(&x, 2u);
            swap(t, t2);
            PPR_ASSERT(t.getTag() == 2u && t2.getTag() == 0u);
        };

        PPR_UNIT_TEST(indexiterator_arithmetic_and_distance) {
            std::array arr = {1, 2, 3, 4};
            IndexIterator<std::array<int, 4>, int> it(arr, 0);
            auto it2 = it + 2;
            PPR_ASSERT((it2 - it) == 2);
            ++it;
            PPR_ASSERT(*it == 2);
        };

        PPR_UNIT_TEST(indexiterator_const_conversion_and_cross_compare) {
            std::array arr = {5, 6};
            IndexIterator<std::array<int, 2>, int> it(arr, 0);
            IndexIterator<std::add_const_t<std::array<int, 2> >, std::add_const_t<int> > cit = it;
            PPR_ASSERT(*cit == 5);
            PPR_ASSERT((it <=> cit) == std::strong_ordering::equal);
        };

        PPR_UNIT_TEST(stack_push_pop_and_iterator) {
            Stack<int, 4> s;
            PPR_ASSERT(s.isEmpty());
            PPR_ASSERT(s.push(10));
            PPR_ASSERT(s.push(20));
            PPR_ASSERT(s.size() == 2u);
            PPR_ASSERT(s.pop().value() == 20);
            int sum = 0;
            for (int v: s.each())
                sum += v;
            PPR_ASSERT(sum == 10);
        };

        PPR_UNIT_TEST(stack_overflow_and_clear) {
            Stack<int, 2> s;
            PPR_ASSERT(s.push(1));
            PPR_ASSERT(s.push(2));
            PPR_ASSERT(!s.push(3));
            s.clear();
            PPR_ASSERT(s.isEmpty());
        };

        PPR_UNIT_TEST(ringbuffer_push_pop_wrap) {
            RingBuffer<int, 3> rb;
            PPR_ASSERT(rb.pushBack(1));
            PPR_ASSERT(rb.pushBack(2));
            PPR_ASSERT(rb.pushBack(3));
            PPR_ASSERT(!rb.pushBack(4));
            PPR_ASSERT(rb.popFront().value() == 1);
            PPR_ASSERT(rb.pushBack(4));
            PPR_ASSERT(rb[0] == 2 && rb[2] == 4);
        };

        PPR_UNIT_TEST(ringbuffer_pop_empty_resets_positions) {
            RingBuffer<int, 2> rb;
            PPR_ASSERT(!rb.popFront().has_value());
            PPR_ASSERT(!rb.popBack().has_value());
        };

        PPR_UNIT_TEST(shellsort_empty_and_single) {
            std::vector<int> empty;
            sort::inplaceShell(empty);
            std::vector single = {42};
            sort::inplaceShell(single);
            PPR_ASSERT(single[0] == 42);
        };

        PPR_UNIT_TEST(shellsort_projection_and_comparator) {
            struct Item {
                int id;
                int key;
            };
            std::vector<Item> v = {{1, 10}, {2, 5}, {3, 7}};
            sort::inplaceShell(v, std::ranges::less{}, &Item::key);
            PPR_ASSERT(v[0].id == 2);
            std::vector desc = {3, 1, 2};
            sort::inplaceShell(desc, std::ranges::greater{});
            PPR_ASSERT(std::ranges::is_sorted(desc, std::ranges::greater{}));
        };

        PPR_UNIT_TEST(hash_mix_64_and_32) {
            const u64 x64 = 0x1234567890abcdefull;
            const u64 m64 = hash::mix(x64);
            PPR_ASSERT(m64 != x64);
            const u32 x32 = 0xdeadbeefu;
            const u32 m32 = hash::mix(x32);
            PPR_ASSERT(m32 != x32);
        };

        PPR_UNIT_TEST(hash_sized_and_unordered_range_and_ptr_combine) {
            static_assert(hash::THashable<u32>);
            std::vector<u32> v = {1, 2, 3};
            auto hs = hash::sizedRange(v);
            auto hs2 = hash::sizedRange(std::vector<u32>{1, 2, 3});
            PPR_ASSERT(hs.m_value == hs2.m_value);
            auto hu = hash::unorderedRange(v);
            auto hu2 = hash::unorderedRange(std::vector<u32>{3, 2, 1});
            PPR_ASSERT(hu.m_value == hu2.m_value);

            int x = 0;
            auto p1 = hash::ptr(&x);
            auto p2 = hash::ptr(&x);
            PPR_ASSERT(p1.m_value == p2.m_value);
            auto c = hash::combine(hash_t{1}, hash_t{2});
            PPR_ASSERT(c.m_value != 0);
        };

        PPR_UNIT_TEST(recycler_allocate_release_and_shrink) {
            Recycler<int, 4> r;
            int out;
            PPR_ASSERT(!r.allocate(out));
            PPR_ASSERT(r.release(42));
            PPR_ASSERT(r.allocate(out));
            PPR_ASSERT(out == 42);
            PPR_VERIFY(r.release(10));
            PPR_VERIFY(r.release(20));
            int destroyed_sum = 0;
            r.shrinkToFit([&](int v) { destroyed_sum += v; });
            PPR_ASSERT(destroyed_sum == 30);
        };

        PPR_UNIT_TEST(additional_hash_and_pointer_checks) {
            auto a = hash::combine(hash_t{5}, hash_t{7});
            auto b = hash::combine(hash_t{7}, hash_t{5});
            PPR_ASSERT(a.m_value != 0 && b.m_value != 0);

            int y = 3;
            RelPtr rp(&y);
            PPR_ASSERT(rp == &y);
            PPR_ASSERT((rp <=> &y) == std::strong_ordering::equal);
        };
    }

    PPR_UNIT_TEST(relocatable) {
        _.recurse(Containers::traits_relocatable_fundamentals);
        _.recurse(Containers::traits_relocatable_user_type_negative);
        _.recurse(Containers::traits_relocatable_specialized_types);
    };

    PPR_UNIT_TEST(bitmask) {
        _.recurse(Containers::bitmask_basic_set_reset_test);
        _.recurse(Containers::bitmask_rotate_and_pop_tests);
        _.recurse(Containers::bitmask_range_and_flip_test);
        _.recurse(Containers::bitmask_byteSwap_and_invert_and_setFirstLastUnsetFirst);
        _.recurse(Containers::bitmask_ref_mutation_visibility);
        _.recurse(Containers::bitmask_ref_compound_ops);
    };

    PPR_UNIT_TEST(pointers) {
        _.recurse(Containers::relptr_null_and_valid);
        _.recurse(Containers::relptr_copy_assign_and_comparisons);
        _.recurse(Containers::tagptr_basic_tag_and_data);
        _.recurse(Containers::tagptr_bits_reinterpret_and_mutation);
    };

    PPR_UNIT_TEST(iterators) {
        _.recurse(Containers::indexiterator_arithmetic_and_distance);
        _.recurse(Containers::indexiterator_const_conversion_and_cross_compare);
    };

    PPR_UNIT_TEST(stack) {
        _.recurse(Containers::stack_push_pop_and_iterator);
        _.recurse(Containers::stack_overflow_and_clear);
    };

    PPR_UNIT_TEST(ring_buffer) {
        _.recurse(Containers::ringbuffer_push_pop_wrap);
        _.recurse(Containers::ringbuffer_pop_empty_resets_positions);
    };

    PPR_UNIT_TEST(sort) {
        _.recurse(Containers::shellsort_empty_and_single);
        _.recurse(Containers::shellsort_projection_and_comparator);
    };

    PPR_UNIT_TEST(hash) {
        _.recurse(Containers::hash_mix_64_and_32);
        _.recurse(Containers::hash_sized_and_unordered_range_and_ptr_combine);
        _.recurse(Containers::additional_hash_and_pointer_checks);
    };

    PPR_UNIT_TEST(recycler) {
        _.recurse(Containers::recycler_allocate_release_and_shrink);
    };
}