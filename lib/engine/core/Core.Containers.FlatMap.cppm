module;
#include "pP/Macros.h"

export module engine.core:containers.flat_map;

import :assert;
import :containers;
import :memory;
import :memory.arena;
import :memory.poison;

import std;

export namespace pP {
    // ------------------------------------------------------------------
    // flat map/set using Eytzinger order for cache-friend binary search
    // ------------------------------------------------------------------

    template<typename KeyT, typename ValueT,
        typename CompareT = std::less<>,
        mem::details::TAllocator AllocatorT = mem::GPA>
    class FlatMap;

    template<typename KeyT,
        typename CompareT = std::less<>,
        mem::details::TAllocator AllocatorT = mem::GPA>
    using FlatSet = FlatMap<KeyT, void, CompareT, AllocatorT>;

    namespace details {
        template<typename KeyT, typename ValueT>
        using flatmap_value_t = std::conditional_t<
            std::is_void_v<ValueT>,
            KeyT,
            std::pair<KeyT, ValueT>
        >;

        template<typename KeyT, typename ValueT = void>
        struct flatmap_reference {
            using type = std::pair<const KeyT &, ValueT &>;

            static constexpr bool has_value_v = true;

            [[nodiscard]] PPR_FORCE_INLINE static constexpr const KeyT &key(const std::pair<KeyT, ValueT> &pair) noexcept {
                return pair.first;
            }
        };

        template<typename KeyT>
        struct flatmap_reference<KeyT, void> {
            using type = const KeyT &;

            static constexpr bool has_value_v = false;

            [[nodiscard]] PPR_FORCE_INLINE static constexpr const KeyT &key(const KeyT &key) noexcept {
                return key;
            }
        };

        template<typename KeyT>
        struct flatmap_reference<KeyT, const void> {
            using type = const KeyT &;

            [[nodiscard]] PPR_FORCE_INLINE static constexpr const KeyT &key(const KeyT &key) noexcept {
                return key;
            }
        };

        template<typename KeyT, typename ValueT>
        using flatmap_reference_t = flatmap_reference<KeyT, ValueT>::type;

        template<typename KeyT, typename ValueT,
            typename CompareT,
            mem::details::TAllocator AllocatorT>
        class FlatMapIterator {
            using map_type = FlatMap<
                KeyT,
                std::remove_const_t<ValueT>,
                CompareT, AllocatorT>;

            friend map_type;
            friend class FlatMapIterator<
                KeyT,
                std::add_const_t<ValueT>,
                CompareT, AllocatorT>;

        public:
            using map_pointer = std::add_pointer_t<std::conditional_t<
                std::is_const_v<ValueT>,
                std::add_const_t<map_type>,
                map_type> >;

            using value_type = flatmap_value_t<KeyT, ValueT>;
            using reference = flatmap_reference_t<KeyT, ValueT>;

            using iterator_category = std::random_access_iterator_tag;
            using iterator_concept = std::random_access_iterator_tag;
            using difference_type = std::ptrdiff_t;

        private:
            class ArrowProxy_ {
                reference m_ref;

                friend FlatMapIterator;

                explicit constexpr ArrowProxy_(const FlatMapIterator &iter) noexcept
                    : m_ref(*iter) {
                }

            public:
                [[nodiscard]] constexpr const reference *operator->() const noexcept {
                    return std::addressof(m_ref);
                }
            };

            map_pointer m_map{nullptr};
            u32 m_index{umax_v};

        public:
            using pointer = ArrowProxy_;

            constexpr FlatMapIterator() noexcept = default;

            constexpr FlatMapIterator(const FlatMapIterator &) noexcept = default;

            constexpr FlatMapIterator &operator=(const FlatMapIterator &) noexcept = default;

            constexpr FlatMapIterator(FlatMapIterator &&) noexcept = default;

            constexpr FlatMapIterator &operator=(FlatMapIterator &&) noexcept = default;

            constexpr FlatMapIterator(map_pointer p_map PPR_LIFETIME_BOUND, const u32 index) noexcept
                : m_map(p_map), m_index(index) {
            }

            using non_const_iterator = FlatMapIterator<KeyT, std::remove_const_t<ValueT>, CompareT, AllocatorT>;

            explicit constexpr FlatMapIterator(const non_const_iterator &other) noexcept
                requires std::is_const_v<ValueT>
                : m_map(other.m_map), m_index(other.m_index) {
            }

            constexpr FlatMapIterator &operator=(const non_const_iterator &other) noexcept
                requires std::is_const_v<ValueT> {
                m_map = other.m_map;
                m_index = other.m_index;
                return *this;
            }

            [[nodiscard]] constexpr bool isValid() const noexcept {
                return m_map != nullptr && m_index < m_map->size();
            }

            [[nodiscard]] constexpr reference operator*() const noexcept {
                PPR_ASSERT(isValid());
                return reference(m_map->m_data[m_index]);
            }

            [[nodiscard]] constexpr pointer operator->() const noexcept {
                PPR_ASSERT(isValid());
                return ArrowProxy_(*this);
            }

            constexpr FlatMapIterator &operator++() noexcept {
                PPR_ASSERT(isValid());
                ++m_index;
                return *this;
            }

            constexpr FlatMapIterator operator++(int) noexcept {
                auto tmp = *this;
                ++m_index;
                return tmp;
            }

            constexpr FlatMapIterator &operator--() noexcept {
                PPR_ASSERT(m_map != nullptr);
                if (m_index == umax_v || m_index >= m_map->size()) {
                    PPR_ASSERT(m_map->size() > 0u);
                    m_index = m_map->size() - 1u;
                } else {
                    PPR_ASSERT(m_index > 0u);
                    --m_index;
                }
                return *this;
            }

            constexpr FlatMapIterator operator--(int) noexcept {
                auto tmp = *this;
                --(*this);
                return tmp;
            }

            constexpr FlatMapIterator &operator+=(const difference_type n) noexcept {
                m_index = safe_narrowing(m_index + n);
                return *this;
            }

            constexpr FlatMapIterator &operator-=(const difference_type n) noexcept {
                m_index = safe_narrowing(m_index - n);
                return *this;
            }

            [[nodiscard]] friend constexpr FlatMapIterator
            operator+(const FlatMapIterator it, const difference_type n) noexcept {
                it += n;
                return it;
            }

            [[nodiscard]] friend constexpr FlatMapIterator
            operator+(const difference_type n, const FlatMapIterator it) noexcept {
                it += n;
                return it;
            }

            [[nodiscard]] friend constexpr FlatMapIterator
            operator-(const FlatMapIterator it, const difference_type n) noexcept {
                it -= n;
                return it;
            }

            [[nodiscard]] friend constexpr difference_type
            operator-(const FlatMapIterator &a, const FlatMapIterator &b) noexcept {
                return static_cast<difference_type>(a.m_index) - static_cast<difference_type>(b.m_index);
            }

            [[nodiscard]] constexpr reference operator[](difference_type n) const noexcept {
                return *(*this + n);
            }

            [[nodiscard]] friend constexpr bool
            operator==(const FlatMapIterator &a, const FlatMapIterator &b) noexcept {
                PPR_ASSERT(a.m_map == b.m_map);
                return a.m_index == b.m_index;
            }

            [[nodiscard]] friend constexpr auto
            operator<=>(const FlatMapIterator &a, const FlatMapIterator &b) noexcept {
                PPR_ASSERT(a.m_map == b.m_map);
                return a.m_index <=> b.m_index;
            }

            [[nodiscard]] friend constexpr bool
            operator==(const FlatMapIterator &it, std::default_sentinel_t) noexcept {
                PPR_ASSERT(it.m_map != nullptr);
                return not it.isValid();
            }
        };
    }

    template<typename KeyT, typename ValueT,
        typename CompareT,
        mem::details::TAllocator AllocatorT>
    class FlatMap : mem::Allocator<AllocatorT> {
        using allocator_type = mem::Allocator<AllocatorT>;

        static_assert(std::is_same_v<KeyT, std::remove_cv_t<KeyT> >,
                      "FlatMap: KeyT must be unqualified");

        static_assert(std::strict_weak_order<CompareT, KeyT, KeyT>,
                      "FlatMap: CompareT must be a strict weak ordering over KeyT");
    public:
        using key_type = KeyT;
        using mapped_type = ValueT;

        using value_type = details::flatmap_value_t<KeyT, ValueT>;
        using size_type = u32;
        using difference_type = std::ptrdiff_t;
        using key_compare = CompareT;

        using reference = details::flatmap_reference_t<KeyT, ValueT>;
        using const_reference = details::flatmap_reference_t<KeyT, const ValueT>;

        using iterator = details::FlatMapIterator<KeyT, std::remove_const_t<ValueT>, CompareT, AllocatorT>;
        using const_iterator = details::FlatMapIterator<KeyT, std::add_const_t<ValueT>, CompareT, AllocatorT>;

        static_assert(std::random_access_iterator<iterator>);
        static_assert(std::random_access_iterator<const_iterator>);

        friend iterator;
        friend const_iterator;

        [[nodiscard]] static constexpr decltype(auto) getKey_(const value_type &value) noexcept {
            return details::flatmap_reference<KeyT, ValueT>::key(value);
        }

    private:
        static constexpr u32 min_capacity_v = 8u;
        static constexpr float growth_factor_v = 1.618f;

        value_type *m_data{nullptr};
        u32 m_capacity{0};
        u32 m_size{0};

        [[nodiscard]] PPR_FORCE_INLINE constexpr AllocatorT &getAllocator_() noexcept {
            return allocator_type::materialize();
        }

        [[nodiscard]] PPR_FORCE_INLINE constexpr const AllocatorT &getAllocator_() const noexcept {
            return allocator_type::materialize();
        }

        [[nodiscard]] constexpr u32 growth_(const u32 wanted) const noexcept {
            const u32 wanted_or_min = std::max(min_capacity_v, wanted);
            const u32 grown = std::max(wanted_or_min, static_cast<u32>(static_cast<float>(m_capacity) * growth_factor_v));
            return std::bit_ceil(grown);
        }

    public:
        constexpr FlatMap() noexcept(std::is_nothrow_default_constructible_v<allocator_type>)
            requires std::is_default_constructible_v<allocator_type> = default;

        constexpr FlatMap(const std::initializer_list<value_type> init)
            requires std::is_default_constructible_v<allocator_type> {
            assignAssumeEmpty(init);
        }

        template<std::forward_iterator Iter>
        constexpr FlatMap(Iter first, Iter last)
            requires std::is_default_constructible_v<allocator_type> {
            assignAssumeEmpty(std::ranges::subrange(first, last));
        }

        explicit constexpr FlatMap(const AllocatorT &al)
            noexcept(std::is_nothrow_copy_constructible_v<allocator_type>)
            requires std::is_copy_constructible_v<allocator_type>
            : allocator_type(al) {
        }

        explicit constexpr FlatMap(AllocatorT &&al)
            noexcept(std::is_nothrow_move_constructible_v<allocator_type>)
            requires std::is_move_constructible_v<allocator_type>
            : allocator_type(std::move(al)) {
        }

        explicit constexpr FlatMap(const u32 initial_capacity)
            requires std::is_default_constructible_v<allocator_type> {
            reserveAssumeEmpty(initial_capacity);
        }

        explicit constexpr FlatMap(const u32 initial_capacity, const AllocatorT &al)
            requires std::is_copy_constructible_v<allocator_type>
            : allocator_type(al) {
            reserveAssumeEmpty(initial_capacity);
        }

        explicit constexpr FlatMap(const u32 initial_capacity, AllocatorT &&al)
            requires std::is_move_constructible_v<allocator_type>
            : allocator_type(std::move(al)) {
            reserveAssumeEmpty(initial_capacity);
        }

        constexpr FlatMap(const FlatMap &other)
            : allocator_type(other.getAllocator_()) {
            assignAssumeEmpty(other);
        }

        constexpr FlatMap &operator=(const FlatMap &other) {
            if (this == &other) [[unlikely]] {
                return *this;
            }

            reset();
            allocator_type::operator=(other.getAllocator_());
            assignAssumeEmpty(other);
            return *this;
        }

        constexpr FlatMap(FlatMap &&other) noexcept
            : allocator_type(std::move(other)) {
            spliceAssumeEmpty_(other);
        }

        constexpr FlatMap &operator=(FlatMap &&other) noexcept {
            if (this == &other) [[unlikely]] {
                return *this;
            }

            reset();
            allocator_type::operator=(std::move(other));
            spliceAssumeEmpty_(other);
            return *this;
        }

        constexpr ~FlatMap() noexcept {
            reset();
        }

        [[nodiscard]] constexpr u32 size() const noexcept {
            return m_size;
        }

        [[nodiscard]] constexpr bool empty() const noexcept {
            return m_size == 0u;
        }

        [[nodiscard]] constexpr u32 capacity() const noexcept {
            return m_capacity;
        }

        [[nodiscard]] std::span<const value_type> view() const noexcept {
            return std::span<const value_type>(m_data, m_size);
        }

        constexpr void reserve(const u32 wanted_capacity) {
            if (wanted_capacity > m_capacity) [[unlikely]] {
                grow_(growth_(wanted_capacity));
            }
        }

        constexpr void reserveAssumeEmpty(const u32 wanted_capacity) {
            PPR_ASSERT(m_size == 0u);
            const u32 new_capacity = std::bit_ceil(std::max(wanted_capacity, min_capacity_v));
            if (new_capacity == m_capacity) [[unlikely]] {
                return;
            }

            if (m_data != nullptr) [[unlikely]] {
                PPR_ASSERT(m_capacity > 0u);
                allocator_type::deallocate(m_data, m_capacity);
            }

            m_capacity = new_capacity;
            m_data = allocator_type::template allocate<value_type>(m_capacity);

            mem::poisonReserved(m_data, m_capacity);
        }

        constexpr void clear() noexcept(std::is_nothrow_destructible_v<value_type>) {
            if (m_size == 0u) [[unlikely]] {
                return;
            }

            PPR_ASSERT(m_capacity > 0u && m_data != nullptr);

            if constexpr (!std::is_trivially_destructible_v<value_type>) {
                std::destroy_n(m_data, m_size);
            }

            mem::poisonReserved(m_data, m_capacity);

            m_size = 0u;
        }

        constexpr void reset() {
            if (m_capacity == 0u) {
                PPR_ASSERT(m_data == nullptr && m_size == 0u);
                return;
            }

            clear();
            allocator_type::deallocate(m_data, m_capacity);

            m_data = nullptr;
            m_capacity = 0u;
            m_size = 0u;
        }

        [[nodiscard]] constexpr iterator begin() noexcept {
            return iterator(this, 0u);
        }

        [[nodiscard]] constexpr std::default_sentinel_t end() noexcept {
            return std::default_sentinel;
        }

        [[nodiscard]] constexpr const_iterator begin() const noexcept {
            return const_iterator(this, 0u);
        }

        [[nodiscard]] constexpr std::default_sentinel_t end() const noexcept {
            return std::default_sentinel;
        }

        [[nodiscard]] constexpr const_iterator cbegin() const noexcept {
            return begin();
        }

        [[nodiscard]] constexpr std::default_sentinel_t cend() const noexcept {
            return std::default_sentinel;
        }

        // lookup

        template<typename KeyLikeT>
        [[nodiscard]] iterator find(const KeyLikeT &key) noexcept
            requires details::TEqualTo<CompareT, KeyT, KeyLikeT> {
            return iterator(this, findEytzinger_(key));
        }

        template<typename KeyLikeT>
        [[nodiscard]] const_iterator find(const KeyLikeT &key) const noexcept
            requires details::TEqualTo<CompareT, KeyT, KeyLikeT> {
            return const_iterator(this, findEytzinger_(key));
        }

        template<typename KeyLikeT>
        [[nodiscard]] iterator lowerBound(const KeyLikeT &key) noexcept
            requires details::TEqualTo<CompareT, KeyT, KeyLikeT> {
            return iterator(this, lowerBoundEytzinger_(key));
        }

        template<typename KeyLikeT>
        [[nodiscard]] const_iterator lowerBound(const KeyLikeT &key) const noexcept
            requires details::TEqualTo<CompareT, KeyT, KeyLikeT> {
            return const_iterator(this, lowerBoundEytzinger_(key));
        }

        template<typename KeyLikeT>
        [[nodiscard]] iterator upperBound(const KeyLikeT &key) noexcept
            requires details::TEqualTo<CompareT, KeyT, KeyLikeT> {
            return iterator(this, upperBoundEytzinger_(key));
        }

        template<typename KeyLikeT>
        [[nodiscard]] const_iterator upperBound(const KeyLikeT &key) const noexcept
            requires details::TEqualTo<CompareT, KeyT, KeyLikeT> {
            return const_iterator(this, upperBoundEytzinger_(key));
        }

        template<typename KeyLikeT>
        [[nodiscard]] constexpr decltype(auto) at(const KeyLikeT &key) const noexcept
            requires details::flatmap_reference<KeyT, ValueT>::has_value_v and
                     details::TEqualTo<CompareT, KeyT, KeyLikeT> {
            auto it = find(key);
            PPR_ASSERT(it != end());
            return it->second;
        }

        template<typename KeyLikeT>
        [[nodiscard]] constexpr decltype(auto) at(const KeyLikeT &key) noexcept
            requires details::flatmap_reference<KeyT, ValueT>::has_value_v and
                     details::TEqualTo<CompareT, KeyT, KeyLikeT> {
            return const_cast<ValueT &>(std::as_const(*this).at(key));
        }

        template<typename KeyLikeT>
        [[nodiscard]] constexpr decltype(auto) operator[](const KeyLikeT &key) noexcept
            requires details::flatmap_reference<KeyT, ValueT>::has_value_v and
                     details::TEqualTo<CompareT, KeyT, KeyLikeT> {
            return at(key);
        }

        template<typename KeyLikeT>
        [[nodiscard]] constexpr bool contains(const KeyLikeT &key) const noexcept
            requires details::TEqualTo<CompareT, KeyT, KeyLikeT> {
            return find(key) != end();
        }

        // mutation (write-rare path: any insert/erase rebuilds the whole Eytzinger layout)

        template<typename... ArgsT>
            requires std::is_constructible_v<value_type, ArgsT...>
        std::pair<iterator, bool> emplace(ArgsT &&... args) {
            return insert(value_type(std::forward<ArgsT>(args)...));
        }

        template<std::convertible_to<value_type> ValueTLike>
        std::pair<iterator, bool> insert(ValueTLike &&value_like) {
            const KeyT &key = getKey_(value_like);

            const auto existing = findEytzinger_(key);
            if (existing < m_size) {
                return {iterator(this, existing), false};
            }

            const KeyT key_copy = key;

            reserve(m_size + 1u);

            auto scoped_arena = mem::ScratchPad::open();
            value_type *const scratch = scoped_arena.allocate<value_type>(m_size + 1u);
            mem::unpoisonUninitialized(scratch, m_size + 1u);

            PPR_DEFER {
                scoped_arena.deallocate(scratch, m_size + 1u);
            };

            u32 constructed = 0u;
            try {
                if (m_size > 0u) [[likely]] {
                    std::uninitialized_move_n(m_data, m_size, scratch);
                    constructed = m_size;
                }
                std::construct_at(&scratch[m_size], std::forward<ValueTLike>(value_like));
                constructed = m_size + 1u;
            } catch (...) {
                std::destroy_n(scratch, constructed);
                throw;
            }

            const u32 new_size = m_size + 1u;

            rebuildEytzingerReplacing_(scratch, m_size, new_size);
            m_size = new_size;

            const auto inserted_pos = findEytzinger_(key_copy);
            PPR_ASSERT(inserted_pos < m_size);
            return {iterator(this, inserted_pos), true};
        }

        template<std::ranges::forward_range RangeT>
            requires std::is_convertible_v<std::ranges::range_value_t<RangeT>, value_type>
        void append(RangeT &&values) {
            const u32 n = checked_cast<u32>(std::ranges::distance(values));
            if (n == 0u) [[unlikely]] {
                return;
            }

            reserve(m_size + n);

            const u32 total = m_size + n;
            auto scoped_arena = mem::ScratchPad::open();
            value_type *const scratch = scoped_arena.allocate<value_type>(total);
            mem::unpoisonUninitialized(scratch, total);

            PPR_DEFER {
                scoped_arena.deallocate(scratch, total);
            };

            u32 constructed = 0u;
            u32 unique_count = 0u;
            try {
                if (m_size > 0u) [[likely]] {
                    std::uninitialized_move_n(m_data, m_size, scratch);
                    constructed = m_size;
                }

                std::ranges::uninitialized_copy(values, std::ranges::subrange(scratch + m_size, scratch + total));
                constructed = total;

                sort::inplaceShell(scratch, scratch + total, keyLess_);

                const auto new_end = std::unique(scratch, scratch + total,
                                                 [](const value_type &a, const value_type &b) noexcept {
                                                     return not keyLess_(a, b) and not keyLess_(b, a);
                                                 });

                unique_count = checked_cast<u32>(new_end - scratch);

                std::destroy(new_end, scratch + total);
                mem::poisonDestroyed(new_end, total - unique_count);
            } catch (...) {
                std::destroy_n(scratch, constructed);
                throw;
            }

            finishRebuild_(scratch, m_size, unique_count);
            m_size = unique_count;
        }

        template<std::forward_iterator Iter>
            requires std::is_convertible_v<typename std::iterator_traits<Iter>::value_type, value_type>
        void append(Iter first, Iter last) {
            append(std::ranges::subrange(first, last));
        }

        template<typename KeyLikeT>
        [[nodiscard]] bool erase(const KeyLikeT &key) noexcept
            requires details::TEqualTo<CompareT, KeyT, KeyLikeT> {
            const auto pos = findEytzinger_(key);
            if (pos >= m_size) {
                return false;
            }
            eraseAt(iterator(this, pos));
            return true;
        }

        void eraseAt(iterator it) {
            PPR_ASSERT(m_size > 0u);
            PPR_ASSERT(it.m_index < m_size);

            const u32 erase_pos = it.m_index;

            if (m_size == 1u) {
                std::destroy_at(&m_data[0]);
                mem::poisonDestroyed(&m_data[0], 1u);
                m_size = 0u;
                return;
            }

            const u32 new_size = m_size - 1u;

            auto scoped_arena = mem::ScratchPad::open();
            value_type *const scratch = scoped_arena.allocate<value_type>(new_size);
            mem::unpoisonUninitialized(scratch, new_size);

            PPR_DEFER {
                scoped_arena.deallocate(scratch, new_size);
            };

            u32 constructed = 0u;
            try {
                std::uninitialized_move(m_data, m_data + erase_pos, scratch);
                constructed = erase_pos;
                std::uninitialized_move(m_data + erase_pos + 1u, m_data + m_size, scratch + erase_pos);
                constructed = new_size;
            } catch (...) {
                std::destroy_n(scratch, constructed);
                throw;
            }

            rebuildEytzingerReplacing_(scratch, m_size, new_size);
            m_size = new_size;
        }

        template<std::ranges::forward_range RangeT>
            requires std::is_convertible_v<std::ranges::range_value_t<RangeT>, value_type>
        void assignAssumeEmpty(RangeT &&values) {
            const u32 n = checked_cast<u32>(std::ranges::distance(values));
            if (n == 0u) [[unlikely]] {
                return;
            }

            reserveAssumeEmpty(n);

            if (n == 1u) {
                mem::unpoisonUninitialized(m_data, 1u);
                std::construct_at(m_data, *std::ranges::begin(values));
                m_size = 1u;
                return;
            }

            auto scoped_arena = mem::ScratchPad::open();
            value_type *const scratch = scoped_arena.allocate<value_type>(n);
            mem::unpoisonUninitialized(scratch, n);
            PPR_DEFER {
                scoped_arena.deallocate(scratch, n);
            };

            try {
                std::ranges::uninitialized_copy(values, std::ranges::subrange(scratch, scratch + n));
            } catch (...) {
                throw;
            }

            rebuildEytzingerReplacing_(scratch, 0u, n);
            m_size = n;
        }

        friend void swap(FlatMap &lhs, FlatMap &rhs) noexcept {
            using namespace std;
            swap(static_cast<allocator_type &>(lhs), static_cast<allocator_type &>(rhs));
            swap(lhs.m_data, rhs.m_data);
            swap(lhs.m_capacity, rhs.m_capacity);
            swap(lhs.m_size, rhs.m_size);
        }

    private:
        [[nodiscard]] static constexpr bool keyLess_(const value_type &a, const value_type &b)
            noexcept(std::is_nothrow_invocable_v<CompareT, const KeyT &, const KeyT &>) {
            return CompareT{}(getKey_(a), getKey_(b));
        }

        void finishRebuild_(value_type *scratch, const u32 old_size, const u32 new_size) noexcept {
            if (old_size > 0u) {
                std::destroy_n(m_data, old_size);
                mem::poisonDestroyed(m_data, old_size);
            }
            buildEytzingerFromSorted_(scratch, new_size);
            std::destroy_n(scratch, new_size);
            mem::poisonDestroyed(scratch, new_size);
        }

        void rebuildEytzingerReplacing_(value_type *scratch, const u32 old_size, const u32 new_size) {
            std::sort(scratch, scratch + new_size, keyLess_);
            finishRebuild_(scratch, old_size, new_size);
        }

        void grow_(const u32 new_capacity) {
            PPR_ASSERT(new_capacity > m_capacity);
            value_type *const new_data = allocator_type::template allocate<value_type>(new_capacity);
            u32 constructed = 0u;

            try {
                if (m_size > 0u) [[likely]] {
                    if constexpr (details::is_relocatable_v<value_type>) {
                        std::memcpy(new_data, m_data, sizeof(value_type) * m_size);
                    } else {
                        for (u32 i = 0u; i < m_size; ++i) {
                            std::construct_at(&new_data[i], std::move(m_data[i]));
                            ++constructed;
                            std::destroy_at(&m_data[i]);
                        }
                    }
                }
            } catch (...) {
                std::destroy_n(new_data, constructed);
                allocator_type::deallocate(new_data, new_capacity);
                throw;
            }

            mem::poisonReserved(new_data + m_size, new_capacity - m_size);

            if (m_data != nullptr) {
                mem::poisonDestroyed(m_data, m_capacity);
                allocator_type::deallocate(m_data, m_capacity);
            }

            m_data = new_data;
            m_capacity = new_capacity;
        }

        void buildEytzingerFromSorted_(value_type *sorted, const u32 n) noexcept {
            mem::unpoisonUninitialized(m_data, n);
            u32 cursor = 0u;
            buildEytzingerImpl_(sorted, n, 0u, cursor);
        }

        void buildEytzingerImpl_(value_type *sorted, const u32 n, const u32 eytz_pos, u32 &cursor) noexcept {
            if (eytz_pos >= n) {
                return;
            }

            buildEytzingerImpl_(sorted, n, 2u * eytz_pos + 1u, cursor);

            if (cursor < n) {
                std::construct_at(&m_data[eytz_pos], std::move(sorted[cursor]));
                ++cursor;
            }

            buildEytzingerImpl_(sorted, n, 2u * eytz_pos + 2u, cursor);
        }

        template<typename KeyLikeT>
        [[nodiscard]] u32 findEytzinger_(const KeyLikeT &key) const noexcept
            requires details::TEqualTo<CompareT, KeyT, KeyLikeT> {
            if (m_size == 0u) [[unlikely]] return umax_v;

            u32 i = 0u;
            while (i < m_size) {
                if (CompareT{}(getKey_(m_data[i]), key)) {
                    i = 2u * i + 2u;
                } else if (CompareT{}(key, getKey_(m_data[i]))) {
                    i = 2u * i + 1u;
                } else {
                    return i;
                }
            }
            return umax_v;
        }

        template<typename KeyLikeT>
        [[nodiscard]] u32 lowerBoundEytzinger_(const KeyLikeT &key) const noexcept
            requires details::TEqualTo<CompareT, KeyT, KeyLikeT> {
            if (m_size == 0u) [[unlikely]] return umax_v;

            u32 i = 0u;
            u32 result = m_size;
            while (i < m_size) {
                if (!CompareT{}(getKey_(m_data[i]), key)) {
                    result = i;
                    i = 2u * i + 1u;
                } else {
                    i = 2u * i + 2u;
                }
            }
            return result;
        }

        template<typename KeyLikeT>
        [[nodiscard]] u32 upperBoundEytzinger_(const KeyLikeT &key) const noexcept
            requires details::TEqualTo<CompareT, KeyT, KeyLikeT> {
            if (m_size == 0u) [[unlikely]] return umax_v;

            u32 i = 0u;
            u32 result = m_size;
            while (i < m_size) {
                if (CompareT{}(key, getKey_(m_data[i]))) {
                    result = i;
                    i = 2u * i + 1u;
                } else {
                    i = 2u * i + 2u;
                }
            }
            return result;
        }

        void spliceAssumeEmpty_(FlatMap &src) noexcept {
            PPR_ASSERT(m_data == nullptr);

            m_data = src.m_data;
            m_capacity = src.m_capacity;
            m_size = src.m_size;

            src.m_data = nullptr;
            src.m_capacity = 0u;
            src.m_size = 0u;
        }
    };

    // relocatable trait

    template<typename KeyT, typename ValueT,
        typename CompareT,
        mem::details::TAllocator AllocatorT>
    struct details::relocatable<FlatMap<KeyT, ValueT, CompareT, AllocatorT> > :
            relocatable<mem::Allocator<AllocatorT> > {
    };

    // deduction guides

    template<typename KeyT, typename ValueT,
        typename CompareT = std::less<KeyT>,
        mem::details::TAllocator AllocatorT = mem::GPA>
    FlatMap(std::initializer_list<std::pair<KeyT, ValueT> >)
        -> FlatMap<KeyT, ValueT, CompareT, AllocatorT>;

    template<std::forward_iterator Iter>
    FlatMap(Iter, Iter)
        -> FlatMap<
            std::remove_cvref_t<decltype(std::declval<Iter>()->first)>,
            std::remove_cvref_t<decltype(std::declval<Iter>()->second)> >;

    // extern template declarations

    extern template class FlatMap<u32, u32>;
    extern template class FlatMap<u64, u64>;
    extern template class FlatMap<u32, float>;

    // ------------------------------------------------------------------
    // flat multi map
    // ------------------------------------------------------------------

    template<typename KeyT, typename ValueT,
        typename CompareT = std::less<>,
        mem::details::TAllocator AllocatorT = mem::GPA>
    class FlatMultiMap;

    namespace details {
        template<typename KeyT, typename ValueT,
            typename CompareT,
            mem::details::TAllocator AllocatorT>
        class FlatMultiMapIterator {
            using map_type = FlatMultiMap<
                KeyT,
                std::remove_const_t<ValueT>,
                CompareT, AllocatorT>;

            friend map_type;
            friend class FlatMultiMapIterator<
                KeyT,
                std::add_const_t<ValueT>,
                CompareT, AllocatorT>;

        public:
            using map_pointer = std::add_pointer_t<std::conditional_t<
                std::is_const_v<ValueT>,
                std::add_const_t<map_type>,
                map_type> >;

            using value_type = flatmap_value_t<KeyT, ValueT>;
            using reference = flatmap_reference_t<KeyT, ValueT>;

            using iterator_category = std::random_access_iterator_tag;
            using iterator_concept = std::random_access_iterator_tag;
            using difference_type = std::ptrdiff_t;

        private:
            class ArrowProxy_ {
                reference m_ref;
                friend FlatMultiMapIterator;

                explicit constexpr ArrowProxy_(const FlatMultiMapIterator &iter) noexcept
                    : m_ref(*iter) {
                }

            public:
                [[nodiscard]] constexpr const reference *operator->() const noexcept {
                    return std::addressof(m_ref);
                }
            };

            map_pointer m_map{nullptr};
            u32 m_index{umax_v};

        public:
            using pointer = ArrowProxy_;

            constexpr FlatMultiMapIterator() noexcept = default;

            constexpr FlatMultiMapIterator(const FlatMultiMapIterator &) noexcept = default;

            constexpr FlatMultiMapIterator &operator=(const FlatMultiMapIterator &) noexcept = default;

            constexpr FlatMultiMapIterator(FlatMultiMapIterator &&) noexcept = default;

            constexpr FlatMultiMapIterator &operator=(FlatMultiMapIterator &&) noexcept = default;

            constexpr FlatMultiMapIterator(map_pointer p_map PPR_LIFETIME_BOUND, const u32 index) noexcept
                : m_map(p_map), m_index(index) {
            }

            using non_const_iterator = FlatMultiMapIterator<KeyT, std::remove_const_t<ValueT>, CompareT, AllocatorT>;

            explicit constexpr FlatMultiMapIterator(const non_const_iterator &other) noexcept
                requires std::is_const_v<ValueT>
                : m_map(other.m_map), m_index(other.m_index) {
            }

            constexpr FlatMultiMapIterator &operator=(const non_const_iterator &other) noexcept
                requires std::is_const_v<ValueT> {
                m_map = other.m_map;
                m_index = other.m_index;
                return *this;
            }

            [[nodiscard]] constexpr bool isValid() const noexcept {
                return m_map != nullptr && m_index < m_map->size();
            }

            [[nodiscard]] constexpr reference operator*() const noexcept {
                PPR_ASSERT(isValid());
                return reference(m_map->m_data[m_index]);
            }

            [[nodiscard]] constexpr pointer operator->() const noexcept {
                PPR_ASSERT(isValid());
                return ArrowProxy_(*this);
            }

            constexpr FlatMultiMapIterator &operator++() noexcept {
                PPR_ASSERT(isValid());
                ++m_index;
                return *this;
            }

            constexpr FlatMultiMapIterator operator++(int) noexcept {
                auto tmp = *this;
                ++m_index;
                return tmp;
            }

            constexpr FlatMultiMapIterator &operator--() noexcept {
                PPR_ASSERT(m_map != nullptr);
                if (m_index == umax_v || m_index >= m_map->size()) {
                    PPR_ASSERT(m_map->size() > 0u);
                    m_index = m_map->size() - 1u;
                } else {
                    PPR_ASSERT(m_index > 0u);
                    --m_index;
                }
                return *this;
            }

            constexpr FlatMultiMapIterator operator--(int) noexcept {
                auto tmp = *this;
                --(*this);
                return tmp;
            }

            constexpr FlatMultiMapIterator &operator+=(difference_type n) noexcept {
                m_index = safe_narrowing(m_index + n);
                return *this;
            }

            constexpr FlatMultiMapIterator &operator-=(difference_type n) noexcept {
                m_index = safe_narrowing(m_index - n);
                return *this;
            }

            [[nodiscard]] friend constexpr FlatMultiMapIterator
            operator+(FlatMultiMapIterator it, difference_type n) noexcept {
                it += n;
                return it;
            }

            [[nodiscard]] friend constexpr FlatMultiMapIterator
            operator+(difference_type n, FlatMultiMapIterator it) noexcept {
                it += n;
                return it;
            }

            [[nodiscard]] friend constexpr FlatMultiMapIterator
            operator-(FlatMultiMapIterator it, difference_type n) noexcept {
                it -= n;
                return it;
            }

            [[nodiscard]] friend constexpr difference_type
            operator-(const FlatMultiMapIterator &a, const FlatMultiMapIterator &b) noexcept {
                return static_cast<difference_type>(a.m_index) - static_cast<difference_type>(b.m_index);
            }

            [[nodiscard]] constexpr reference operator[](difference_type n) const noexcept {
                return *(*this + n);
            }

            [[nodiscard]] friend constexpr bool operator==(const FlatMultiMapIterator &a, const FlatMultiMapIterator &b) noexcept {
                PPR_ASSERT(a.m_map == b.m_map);
                return a.m_index == b.m_index;
            }

            [[nodiscard]] friend constexpr auto operator<=>(const FlatMultiMapIterator &a, const FlatMultiMapIterator &b) noexcept {
                PPR_ASSERT(a.m_map == b.m_map);
                return a.m_index <=> b.m_index;
            }

            [[nodiscard]] friend constexpr bool operator==(const FlatMultiMapIterator &it, std::default_sentinel_t) noexcept {
                PPR_ASSERT(it.m_map != nullptr);
                return not it.isValid();
            }
        };
    }

    template<typename KeyT, typename ValueT,
        typename CompareT,
        mem::details::TAllocator AllocatorT>
    class FlatMultiMap : mem::Allocator<AllocatorT> {
        using allocator_type = mem::Allocator<AllocatorT>;

        static_assert(std::is_same_v<KeyT, std::remove_cv_t<KeyT> >,
                      "FlatMultiMap: KeyT must be unqualified");
        static_assert(std::strict_weak_order<CompareT, KeyT, KeyT>,
                      "FlatMultiMap: CompareT must be a strict weak ordering over KeyT");
        static_assert(!std::is_void_v<ValueT>,
                      "FlatMultiMap: ValueT=void (flat-multiset) not implemented yet.");

    public:
        using key_type = KeyT;
        using mapped_type = ValueT;
        using value_type = details::flatmap_value_t<KeyT, ValueT>;
        using size_type = u32;
        using difference_type = std::ptrdiff_t;
        using key_compare = CompareT;

        using reference = details::flatmap_reference_t<KeyT, ValueT>;
        using const_reference = details::flatmap_reference_t<KeyT, const ValueT>;

        using iterator = details::FlatMultiMapIterator<KeyT, std::remove_const_t<ValueT>, CompareT, AllocatorT>;
        using const_iterator = details::FlatMultiMapIterator<KeyT, std::add_const_t<ValueT>, CompareT, AllocatorT>;

        static_assert(std::random_access_iterator<iterator>);
        static_assert(std::random_access_iterator<const_iterator>);

        friend iterator;
        friend const_iterator;

    private:
        static constexpr u32 min_capacity_v = 8u;
        static constexpr float growth_factor_v = 1.618f;

        value_type *m_data{nullptr};
        u32 m_capacity{0};
        u32 m_size{0};

        [[nodiscard]] PPR_FORCE_INLINE constexpr AllocatorT &getAllocator_() noexcept { return allocator_type::materialize(); }
        [[nodiscard]] PPR_FORCE_INLINE constexpr const AllocatorT &getAllocator_() const noexcept { return allocator_type::materialize(); }

        [[nodiscard]] constexpr u32 growth_(const u32 wanted) const noexcept {
            const u32 wanted_or_min = std::max(min_capacity_v, wanted);
            const u32 grown = std::max(wanted_or_min, static_cast<u32>(static_cast<float>(m_capacity) * growth_factor_v));
            return std::bit_ceil(grown);
        }

    public:
        constexpr FlatMultiMap() noexcept(std::is_nothrow_default_constructible_v<allocator_type>)
            requires std::is_default_constructible_v<allocator_type> = default;

        constexpr FlatMultiMap(const std::initializer_list<value_type> init)
            requires std::is_default_constructible_v<allocator_type> {
            assignAssumeEmpty(init);
        }

        template<std::forward_iterator Iter>
        constexpr FlatMultiMap(Iter first, Iter last)
            requires std::is_default_constructible_v<allocator_type> {
            assignAssumeEmpty(std::ranges::subrange(first, last));
        }

        explicit constexpr FlatMultiMap(const AllocatorT &al)
            noexcept(std::is_nothrow_copy_constructible_v<allocator_type>)
            requires std::is_copy_constructible_v<allocator_type> : allocator_type(al) {
        }

        explicit constexpr FlatMultiMap(AllocatorT &&al)
            noexcept(std::is_nothrow_move_constructible_v<allocator_type>)
            requires std::is_move_constructible_v<allocator_type> : allocator_type(std::move(al)) {
        }

        constexpr FlatMultiMap(const FlatMultiMap &other) : allocator_type(other.getAllocator_()) {
            assignAssumeEmpty(other);
        }

        constexpr FlatMultiMap &operator=(const FlatMultiMap &other) {
            if (this == &other) [[unlikely]] return *this;
            reset();
            allocator_type::operator=(other.getAllocator_());
            assignAssumeEmpty(other);
            return *this;
        }

        constexpr FlatMultiMap(FlatMultiMap &&other) noexcept : allocator_type(std::move(other)) {
            spliceAssumeEmpty_(other);
        }

        constexpr FlatMultiMap &operator=(FlatMultiMap &&other) noexcept {
            if (this == &other) [[unlikely]] return *this;
            reset();
            allocator_type::operator=(std::move(other));
            spliceAssumeEmpty_(other);
            return *this;
        }

        constexpr ~FlatMultiMap() noexcept { reset(); }

        [[nodiscard]] constexpr u32 size() const noexcept { return m_size; }
        [[nodiscard]] constexpr bool empty() const noexcept { return m_size == 0u; }
        [[nodiscard]] constexpr u32 capacity() const noexcept { return m_capacity; }

        [[nodiscard]] std::span<const value_type> view() const noexcept {
            return std::span<const value_type>(m_data, m_size);
        }

        constexpr void reserve(const u32 wanted_capacity) {
            if (wanted_capacity > m_capacity) [[unlikely]] {
                grow_(growth_(wanted_capacity));
            }
        }

        constexpr void reserveAssumeEmpty(const u32 wanted_capacity) {
            PPR_ASSERT(m_size == 0u);
            const u32 new_capacity = std::bit_ceil(std::max(wanted_capacity, min_capacity_v));
            if (new_capacity == m_capacity) [[unlikely]] return;

            if (m_data != nullptr) [[unlikely]] {
                PPR_ASSERT(m_capacity > 0u);
                allocator_type::deallocate(m_data, m_capacity);
            }

            m_capacity = new_capacity;
            m_data = allocator_type::template allocate<value_type>(m_capacity);
            mem::poisonReserved(m_data, m_capacity);
        }

        constexpr void clear() noexcept(std::is_nothrow_destructible_v<value_type>) {
            if (m_size == 0u) [[unlikely]] return;
            PPR_ASSERT(m_capacity > 0u && m_data != nullptr);

            if constexpr (!std::is_trivially_destructible_v<value_type>) {
                std::destroy_n(m_data, m_size);
            }
            mem::poisonReserved(m_data, m_capacity);
            m_size = 0u;
        }

        constexpr void reset() {
            if (m_capacity == 0u) {
                PPR_ASSERT(m_data == nullptr && m_size == 0u);
                return;
            }
            clear();
            allocator_type::deallocate(m_data, m_capacity);
            m_data = nullptr;
            m_capacity = 0u;
            m_size = 0u;
        }

        [[nodiscard]] constexpr iterator begin() noexcept { return iterator(this, 0u); }
        [[nodiscard]] constexpr std::default_sentinel_t end() noexcept { return std::default_sentinel; }
        [[nodiscard]] constexpr const_iterator begin() const noexcept { return const_iterator(this, 0u); }
        [[nodiscard]] constexpr std::default_sentinel_t end() const noexcept { return std::default_sentinel; }
        [[nodiscard]] constexpr const_iterator cbegin() const noexcept { return begin(); }
        [[nodiscard]] constexpr std::default_sentinel_t cend() const noexcept { return std::default_sentinel; }

        // --- LOOKUP (Using Standard Binary Search on Flat Array) ---

        template<typename KeyLikeT>
        [[nodiscard]] iterator find(const KeyLikeT &key) noexcept
            requires details::TEqualTo<CompareT, KeyT, KeyLikeT> {
            const u32 idx = lowerBoundIndex_(key);
            if (idx < m_size && !CompareT{}(key, m_data[idx].first)) {
                return iterator(this, idx);
            }
            return iterator(this, m_size); // basically end()
        }

        template<typename KeyLikeT>
        [[nodiscard]] const_iterator find(const KeyLikeT &key) const noexcept
            requires details::TEqualTo<CompareT, KeyT, KeyLikeT> {
            const u32 idx = lowerBoundIndex_(key);
            if (idx < m_size && !CompareT{}(key, m_data[idx].first)) {
                return const_iterator(this, idx);
            }
            return const_iterator(this, m_size);
        }

        template<typename KeyLikeT>
        [[nodiscard]] iterator lowerBound(const KeyLikeT &key) noexcept
            requires details::TEqualTo<CompareT, KeyT, KeyLikeT> {
            return iterator(this, lowerBoundIndex_(key));
        }

        template<typename KeyLikeT>
        [[nodiscard]] const_iterator lowerBound(const KeyLikeT &key) const noexcept
            requires details::TEqualTo<CompareT, KeyT, KeyLikeT> {
            return const_iterator(this, lowerBoundIndex_(key));
        }

        template<typename KeyLikeT>
        [[nodiscard]] iterator upperBound(const KeyLikeT &key) noexcept
            requires details::TEqualTo<CompareT, KeyT, KeyLikeT> {
            return iterator(this, upperBoundIndex_(key));
        }

        template<typename KeyLikeT>
        [[nodiscard]] const_iterator upperBound(const KeyLikeT &key) const noexcept
            requires details::TEqualTo<CompareT, KeyT, KeyLikeT> {
            return const_iterator(this, upperBoundIndex_(key));
        }

        template<typename KeyLikeT>
        [[nodiscard]] std::pair<iterator, iterator> equal_range(const KeyLikeT &key) noexcept
            requires details::TEqualTo<CompareT, KeyT, KeyLikeT> {
            return {lowerBound(key), upperBound(key)};
        }

        template<typename KeyLikeT>
        [[nodiscard]] std::pair<const_iterator, const_iterator> equal_range(const KeyLikeT &key) const noexcept
            requires details::TEqualTo<CompareT, KeyT, KeyLikeT> {
            return {lowerBound(key), upperBound(key)};
        }

        template<typename KeyLikeT>
        [[nodiscard]] u32 count(const KeyLikeT &key) const noexcept
            requires details::TEqualTo<CompareT, KeyT, KeyLikeT> {
            const u32 lower = lowerBoundIndex_(key);
            if (lower >= m_size || CompareT{}(key, m_data[lower].first))
                return 0u;
            return upperBoundIndex_(key) - lower;
        }

        template<typename KeyLikeT>
        [[nodiscard]] constexpr bool contains(const KeyLikeT &key) const noexcept
            requires details::TEqualTo<CompareT, KeyT, KeyLikeT> {
            return count(key) > 0;
        }

        // --- MUTATION ---

        template<typename... ArgsT>
            requires std::is_constructible_v<value_type, ArgsT...>
        iterator emplace(ArgsT &&... args) {
            return insert(value_type(std::forward<ArgsT>(args)...));
        }

        template<std::convertible_to<value_type> ValueTLike>
        iterator insert(ValueTLike &&value_like) {
            reserve(m_size + 1u);
            const KeyT key_copy = value_like.first;

            auto scoped_arena = mem::ScratchPad::open();
            value_type *const scratch = scoped_arena.allocate<value_type>(m_size + 1u);
            mem::unpoisonUninitialized(scratch, m_size + 1u);

            PPR_DEFER { scoped_arena.deallocate(scratch, m_size + 1u); };

            u32 constructed = 0u;
            try {
                if (m_size > 0u) [[likely]] {
                    std::uninitialized_move_n(m_data, m_size, scratch);
                    constructed = m_size;
                }
                std::construct_at(&scratch[m_size], std::forward<ValueTLike>(value_like));
                constructed = m_size + 1u;
            } catch (...) {
                std::destroy_n(scratch, constructed);
                throw;
            }

            const u32 new_size = m_size + 1u;
            rebuildReplacing_(scratch, m_size, new_size);
            m_size = new_size;

            return upperBound(key_copy); // Returns an iterator to the newly inserted stable element
        }

        template<std::ranges::forward_range RangeT>
            requires std::is_convertible_v<std::ranges::range_value_t<RangeT>, value_type>
        void append(RangeT &&values) {
            const u32 n = checked_cast<u32>(std::ranges::distance(values));
            if (n == 0u) [[unlikely]] return;

            reserve(m_size + n);
            const u32 total = m_size + n;
            auto scoped_arena = mem::ScratchPad::open();
            value_type *const scratch = scoped_arena.allocate<value_type>(total);
            mem::unpoisonUninitialized(scratch, total);

            PPR_DEFER { scoped_arena.deallocate(scratch, total); };

            u32 constructed = 0u;
            try {
                if (m_size > 0u) [[likely]] {
                    std::uninitialized_move_n(m_data, m_size, scratch);
                    constructed = m_size;
                }
                std::ranges::uninitialized_copy(values, std::ranges::subrange(scratch + m_size, scratch + total));
                constructed = total;

                // For MultiMap, we use stable_sort to keep relative order of duplicate insertions intact
                std::stable_sort(scratch, scratch + total, keyLess_);
            } catch (...) {
                std::destroy_n(scratch, constructed);
                throw;
            }

            finishRebuild_(scratch, m_size, total);
            m_size = total;
        }

        template<std::forward_iterator Iter>
            requires std::is_convertible_v<typename std::iterator_traits<Iter>::value_type, value_type>
        void append(Iter first, Iter last) {
            append(std::ranges::subrange(first, last));
        }

        template<typename KeyLikeT>
        [[nodiscard]] u32 erase(const KeyLikeT &key) noexcept
            requires details::TEqualTo<CompareT, KeyT, KeyLikeT> {
            const u32 first_idx = lowerBoundIndex_(key);
            const u32 last_idx = upperBoundIndex_(key);

            if (first_idx >= m_size || first_idx == last_idx) {
                return 0u;
            }

            const u32 erased_count = last_idx - first_idx;
            eraseRange_(first_idx, last_idx);
            return erased_count;
        }

        void eraseAt(iterator it) {
            PPR_ASSERT(m_size > 0u);
            PPR_ASSERT(it.m_index < m_size);
            eraseRange_(it.m_index, it.m_index + 1u);
        }

        template<std::ranges::forward_range RangeT>
            requires std::is_convertible_v<std::ranges::range_value_t<RangeT>, value_type>
        void assignAssumeEmpty(RangeT &&values) {
            const u32 n = checked_cast<u32>(std::ranges::distance(values));
            if (n == 0u) [[unlikely]] return;

            reserveAssumeEmpty(n);

            if (n == 1u) {
                mem::unpoisonUninitialized(m_data, 1u);
                std::construct_at(m_data, *std::ranges::begin(values));
                m_size = 1u;
                return;
            }

            auto scoped_arena = mem::ScratchPad::open();
            value_type *const scratch = scoped_arena.allocate<value_type>(n);
            mem::unpoisonUninitialized(scratch, n);
            PPR_DEFER { scoped_arena.deallocate(scratch, n); };

            try {
                std::ranges::uninitialized_copy(values, std::ranges::subrange(scratch, scratch + n));
            } catch (...) { throw; }

            rebuildReplacing_(scratch, 0u, n);
            m_size = n;
        }

        friend void swap(FlatMultiMap &lhs, FlatMultiMap &rhs) noexcept {
            using namespace std;
            swap(static_cast<allocator_type &>(lhs), static_cast<allocator_type &>(rhs));
            swap(lhs.m_data, rhs.m_data);
            swap(lhs.m_capacity, rhs.m_capacity);
            swap(lhs.m_size, rhs.m_size);
        }

    private:
        [[nodiscard]] static constexpr bool keyLess_(const value_type &a, const value_type &b)
            noexcept(std::is_nothrow_invocable_v<CompareT, const KeyT &, const KeyT &>) {
            return CompareT{}(a.first, b.first);
        }

        template<typename KeyLikeT>
        [[nodiscard]] u32 lowerBoundIndex_(const KeyLikeT &key) const noexcept {
            if (m_size == 0u) [[unlikely]] return umax_v;
            auto it = std::lower_bound(m_data, m_data + m_size, key,
                                       [](const value_type &a, const KeyLikeT &b) {
                                           return CompareT{}(a.first, b);
                                       });
            return it == m_data + m_size ? m_size : static_cast<u32>(it - m_data);
        }

        template<typename KeyLikeT>
        [[nodiscard]] u32 upperBoundIndex_(const KeyLikeT &key) const noexcept {
            if (m_size == 0u) [[unlikely]] return umax_v;
            auto it = std::upper_bound(m_data, m_data + m_size, key,
                                       [](const KeyLikeT &a, const value_type &b) {
                                           return CompareT{}(a, b.first);
                                       });
            return it == m_data + m_size ? m_size : static_cast<u32>(it - m_data);
        }

        void finishRebuild_(value_type *scratch, const u32 old_size, const u32 new_size) noexcept {
            if (old_size > 0u) {
                std::destroy_n(m_data, old_size);
                mem::poisonDestroyed(m_data, old_size);
            }
            mem::unpoisonUninitialized(m_data, new_size);

            // Relocate exactly matching the linear structure
            for (u32 i = 0u; i < new_size; ++i) {
                std::construct_at(&m_data[i], std::move(scratch[i]));
            }

            std::destroy_n(scratch, new_size);
            mem::poisonDestroyed(scratch, new_size);
        }

        void rebuildReplacing_(value_type *scratch, const u32 old_size, const u32 new_size) {
            std::stable_sort(scratch, scratch + new_size, keyLess_);
            finishRebuild_(scratch, old_size, new_size);
        }

        void eraseRange_(const u32 first_idx, const u32 last_idx) {
            const u32 count = last_idx - first_idx;
            const u32 new_size = m_size - count;

            if (new_size == 0u) {
                clear();
                return;
            }

            auto scoped_arena = mem::ScratchPad::open();
            value_type *const scratch = scoped_arena.allocate<value_type>(new_size);
            mem::unpoisonUninitialized(scratch, new_size);
            PPR_DEFER { scoped_arena.deallocate(scratch, new_size); };

            u32 constructed = 0u;
            try {
                std::uninitialized_move(m_data, m_data + first_idx, scratch);
                constructed = first_idx;
                std::uninitialized_move(m_data + last_idx, m_data + m_size, scratch + first_idx);
                constructed = new_size;
            } catch (...) {
                std::destroy_n(scratch, constructed);
                throw;
            }

            finishRebuild_(scratch, m_size, new_size); // Bypasses sort, since remaining data is already sorted
            m_size = new_size;
        }

        void grow_(const u32 new_capacity) {
            PPR_ASSERT(new_capacity > m_capacity);
            value_type *const new_data = allocator_type::template allocate<value_type>(new_capacity);
            u32 constructed = 0u;

            try {
                if (m_size > 0u) [[likely]] {
                    if constexpr (details::is_relocatable_v<value_type>) {
                        std::memcpy(new_data, m_data, sizeof(value_type) * m_size);
                    } else {
                        for (u32 i = 0u; i < m_size; ++i) {
                            std::construct_at(&new_data[i], std::move(m_data[i]));
                            ++constructed;
                            std::destroy_at(&m_data[i]);
                        }
                    }
                }
            } catch (...) {
                std::destroy_n(new_data, constructed);
                allocator_type::deallocate(new_data, new_capacity);
                throw;
            }

            mem::poisonReserved(new_data + m_size, new_capacity - m_size);

            if (m_data != nullptr) {
                mem::poisonDestroyed(m_data, m_capacity);
                allocator_type::deallocate(m_data, m_capacity);
            }

            m_data = new_data;
            m_capacity = new_capacity;
        }

        void spliceAssumeEmpty_(FlatMultiMap &src) noexcept {
            PPR_ASSERT(m_data == nullptr);
            m_data = src.m_data;
            m_capacity = src.m_capacity;
            m_size = src.m_size;

            src.m_data = nullptr;
            src.m_capacity = 0u;
            src.m_size = 0u;
        }
    };

    // relocatable trait
    template<typename KeyT, typename ValueT,
        typename CompareT,
        mem::details::TAllocator AllocatorT>
    struct details::relocatable<FlatMultiMap<KeyT, ValueT, CompareT, AllocatorT> > :
            relocatable<mem::Allocator<AllocatorT> > {
    };

    // deduction guides
    template<typename KeyT, typename ValueT,
        typename CompareT = std::less<KeyT>,
        mem::details::TAllocator AllocatorT = mem::GPA>
    FlatMultiMap(std::initializer_list<std::pair<KeyT, ValueT> >)
        -> FlatMultiMap<KeyT, ValueT, CompareT, AllocatorT>;

    template<std::forward_iterator Iter>
    FlatMultiMap(Iter, Iter)
        -> FlatMultiMap<
            std::remove_cvref_t<decltype(std::declval<Iter>()->first)>,
            std::remove_cvref_t<decltype(std::declval<Iter>()->second)> >;
}
