module;
#include "pP/Macros.h"
export module engine.core:containers.stl;

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

    // ------------------------------------------------------------------
    // useful helpers for common STL allocators
    // ------------------------------------------------------------------

    namespace details {
        template<std::three_way_comparable T>
        struct PriorityPair {
            int m_key{0};
            T m_value{};

            [[nodiscard]] auto &operator*(this auto self) noexcept {
                return self.m_value;
            }

            [[nodiscard]] auto *operator->(this auto self) noexcept {
                return std::addressof(self.m_value);
            }

            [[nodiscard]] bool operator==(const T &item) const noexcept {
                return m_value == item;
            }

            [[nodiscard]] auto operator<=>(const T &item) const noexcept {
                return m_value <=> item;
            }

            [[nodiscard]] bool operator==(const PriorityPair &other) const noexcept {
                return m_key == other.m_key and m_value == other.m_value;
            }

            [[nodiscard]] auto operator<=>(const PriorityPair &other) const noexcept {
                if (const std::strong_ordering cmp_priority = m_key <=> other.m_key;
                    cmp_priority == std::strong_ordering::equal) {
                    return m_value <=> other.m_value;
                } else {
                    return cmp_priority;
                }
            }
        };
    }

    template<std::three_way_comparable T, mem::details::TAllocator AllocatorT = mem::GPA>
    using PrioritySet = FlatSet<details::PriorityPair<T>, std::less<>, AllocatorT>;
}
