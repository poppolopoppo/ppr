module;
#include "pP/Macros.h"
export module engine.core:containers.stl;

import :assert;
import :memory;
import :memory.allocator;
import std;

export namespace pP {
    // ------------------------------------------------------------------
    // template aliases for common STL allocators
    // ------------------------------------------------------------------

    template<
        typename T,
        mem::details::TAllocator AllocatorT = mem::GPA>
    using Array = std::vector<T, mem::STL<T, AllocatorT> >;

    template<
        typename T,
        mem::details::TAllocator AllocatorT = mem::GPA>
    using Deque = std::deque<T, mem::STL<T, AllocatorT> >;

    template<
        typename KeyT,
        typename CompareT = std::less<>,
        mem::details::TAllocator AllocatorT = mem::GPA>
        requires std::is_invocable_r_v<bool, CompareT, const KeyT &, const KeyT &>
    using FlatSet = std::flat_set<
        KeyT,
        CompareT,
        Array<KeyT, AllocatorT>
    >;

    template<
        typename KeyT, typename ValueT,
        typename CompareT = std::less<>,
        mem::details::TAllocator AllocatorT = mem::GPA>
        requires std::is_invocable_r_v<bool, CompareT, const KeyT &, const KeyT &>
    using FlatMap = std::flat_map<
        KeyT, ValueT,
        CompareT,
        Array<KeyT, AllocatorT>,
        Array<ValueT, AllocatorT>
    >;

    template<
        typename KeyT,
        typename CompareT = std::less<>,
        mem::details::TAllocator AllocatorT = mem::GPA>
        requires std::is_invocable_r_v<bool, CompareT, const KeyT &, const KeyT &>
    using FlatMultiSet = std::flat_multiset<
        KeyT,
        CompareT,
        Array<KeyT, AllocatorT>
    >;

    template<
        typename KeyT, typename ValueT,
        typename CompareT = std::less<>,
        mem::details::TAllocator AllocatorT = mem::GPA>
        requires std::is_invocable_r_v<bool, CompareT, const KeyT &, const KeyT &>
    using FlatMultiMap = std::flat_multimap<
        KeyT, ValueT,
        CompareT,
        Array<KeyT, AllocatorT>,
        Array<ValueT, AllocatorT>
    >;
}
