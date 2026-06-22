module;
#include "pP/Macros.h"

export module engine.core:containers.flat_map;

import :assert;
import :containers;
import :memory;
import :memory.poison;

import std;

export namespace pP {
    template<typename KeyT, typename ValueT,
        typename CompareT = std::less<KeyT>,
        mem::details::TAllocator AllocatorT = mem::GPA>
    class FlatMap;

    namespace details {
        template<typename KeyT, typename ValueT>
        using flatmap_value_t = std::pair<KeyT, ValueT>;

        template<typename KeyT, typename ValueT = void>
        struct flatmap_reference {
            using type = std::pair<const KeyT &, ValueT &>;
        };

        template<typename KeyT, typename ValueT,
            typename CompareT,
            mem::details::TAllocator AllocatorT>
        class FlatMapIterator {
            using map_type = FlatMap<
                KeyT,
                std::remove_const_t<ValueT>,
                CompareT, AllocatorT>;
            using internal_pair = flatmap_value_t<KeyT, std::remove_const_t<ValueT> >;
            using user_pair = std::pair<const KeyT, ValueT>;

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

            using value_type = user_pair;
            using reference = std::conditional_t<
                std::is_const_v<ValueT>,
                const user_pair &,
                user_pair &>;
            using pointer = std::add_pointer_t<reference>;

            using iterator_category = std::bidirectional_iterator_tag;
            using iterator_concept = std::bidirectional_iterator_tag;
            using difference_type = std::ptrdiff_t;

        private:
            class ArrowProxy_ {
                user_pair m_value{};

                friend FlatMapIterator;

                explicit constexpr ArrowProxy_(const FlatMapIterator &iter) noexcept
                    : m_value(*iter) {
                }

            public:
                [[nodiscard]] constexpr pointer operator->() noexcept {
                    return std::addressof(m_value);
                }
            };

            map_pointer m_map{nullptr};
            u32 m_index{umax_v};

        public:
            using arrow_proxy = ArrowProxy_;

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
                static_assert(sizeof(internal_pair) == sizeof(user_pair));
                static_assert(alignof(internal_pair) == alignof(user_pair));
                return *std::launder(reinterpret_cast<user_pair *>(std::addressof(m_map->m_data[m_index])));
            }

            [[nodiscard]] constexpr ArrowProxy_ operator->() const noexcept {
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
                ++(*this);
                return tmp;
            }

            constexpr FlatMapIterator &operator--() noexcept {
                PPR_ASSERT(isValid());
                PPR_ASSERT(m_index > 0u);
                --m_index;
                return *this;
            }

            constexpr FlatMapIterator operator--(int) noexcept {
                auto tmp = *this;
                --(*this);
                return tmp;
            }

            [[nodiscard]] friend constexpr bool
            operator==(const FlatMapIterator &a, const FlatMapIterator &b) noexcept {
                PPR_ASSERT(a.m_map == b.m_map);
                return a.m_index == b.m_index;
            }

            [[nodiscard]] friend constexpr std::strong_ordering
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
        using internal_pair = details::flatmap_value_t<KeyT, ValueT>;
        using user_pair = std::pair<const KeyT, ValueT>;

        static_assert(std::is_same_v<KeyT, std::remove_cv_t<KeyT> >,
                      "FlatMap: KeyT must be unqualified");
        static_assert(std::strict_weak_order<CompareT, KeyT, KeyT>,
                      "FlatMap: CompareT must be a strict weak ordering over KeyT");

        template<typename T>
        using wrap_const = std::conditional_t<std::is_const_v<ValueT>, std::add_const_t<T>, T>;

    public:
        using key_type = KeyT;
        using mapped_type = ValueT;
        using value_type = user_pair;
        using size_type = u32;
        using difference_type = std::ptrdiff_t;
        using key_compare = CompareT;

        using reference = wrap_const<user_pair> &;
        using const_reference = const user_pair &;

        using iterator = details::FlatMapIterator<KeyT, std::remove_const_t<ValueT>, CompareT, AllocatorT>;
        using const_iterator = details::FlatMapIterator<KeyT, std::add_const_t<ValueT>, CompareT, AllocatorT>;

        static_assert(std::bidirectional_iterator<iterator>);
        static_assert(std::bidirectional_iterator<const_iterator>);

        friend iterator;
        friend const_iterator;

    private:
        static constexpr u32 min_capacity_v = 8u;
        static constexpr float growth_factor_v = 1.618f;

        internal_pair *m_data{nullptr};
        u32 m_capacity{0};
        u32 m_size{0};

        [[nodiscard]] PPR_FORCE_INLINE constexpr AllocatorT &getAllocator_() noexcept {
            return allocator_type::materialize();
        }

        [[nodiscard]] PPR_FORCE_INLINE constexpr const AllocatorT &getAllocator_() const noexcept {
            return allocator_type::materialize();
        }

        [[nodiscard]] constexpr u32 growth_(const u32 wanted) const noexcept {
            u32 n = std::max(min_capacity_v, wanted);
            n = std::max(n, static_cast<u32>(static_cast<float>(m_capacity) * growth_factor_v));
            u32 pow2 = std::bit_ceil(n);
            return pow2;
        }

    public:
        constexpr FlatMap() noexcept(std::is_nothrow_default_constructible_v<allocator_type>)
            requires std::is_default_constructible_v<allocator_type> = default;

        constexpr FlatMap(std::initializer_list<internal_pair> init)
            requires std::is_default_constructible_v<allocator_type> {
            if (init.size() == 0u) {
                return;
            }
            reserveAssumeEmpty(checked_cast<u32>(init.size()));

            auto *const temp = allocator_type::template allocate<internal_pair>(init.size());
            mem::unpoisonUninitialized(temp, init.size());

            u32 i = 0u;
            for (const auto &kv: init) {
                std::construct_at(&temp[i], kv);
                ++i;
            }

            std::sort(temp, temp + init.size(),
                      [](const internal_pair &a, const internal_pair &b) {
                          return CompareT{}(a.first, b.first);
                      });

            buildEytzingerFromSorted_(temp, checked_cast<u32>(init.size()));
            std::destroy_n(temp, init.size());
            allocator_type::deallocate(temp, checked_cast<u32>(init.size()));
            m_size = checked_cast<u32>(init.size());
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
            spliceAssumeEmpty(other);
        }

        constexpr FlatMap &operator=(FlatMap &&other) noexcept {
            if (this == &other) [[unlikely]] {
                return *this;
            }

            reset();
            allocator_type::operator=(std::move(other));
            spliceAssumeEmpty(other);
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
                if constexpr (!std::is_trivially_destructible_v<internal_pair>) {
                    if (m_size > 0u) {
                        std::destroy_n(m_data, m_size);
                        mem::poisonDestroyed(m_data, m_size);
                    }
                }
                allocator_type::deallocate(m_data, m_capacity);
            }

            m_capacity = new_capacity;
            m_data = allocator_type::template allocate<internal_pair>(m_capacity);

            mem::poisonReserved(m_data, m_capacity);
        }

        constexpr void clear() noexcept(std::is_nothrow_destructible_v<user_pair>) {
            if (m_size == 0u) [[unlikely]] {
                return;
            }

            PPR_ASSERT(m_capacity > 0u && m_data != nullptr);

            if constexpr (!std::is_trivially_destructible_v<internal_pair>) {
                std::destroy_n(m_data, m_size);
                mem::poisonDestroyed(m_data, m_size);
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

            PPR_ASSERT(m_size == 0u);
            mem::poisonDestroyed(m_data, m_capacity);
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
        [[nodiscard]] constexpr ValueT &operator[](const KeyLikeT &key) noexcept
            requires details::TEqualTo<CompareT, KeyT, KeyLikeT> {
            auto it = find(key);
            PPR_ASSERT(it != end());
            return it->second;
        }

        template<typename KeyLikeT>
        [[nodiscard]] constexpr const ValueT &at(const KeyLikeT &key) const noexcept
            requires details::TEqualTo<CompareT, KeyT, KeyLikeT> {
            auto it = find(key);
            PPR_ASSERT(it != end());
            return it->second;
        }

        template<typename KeyLikeT>
        [[nodiscard]] constexpr ValueT &at(const KeyLikeT &key) noexcept
            requires details::TEqualTo<CompareT, KeyT, KeyLikeT> {
            auto it = find(key);
            PPR_ASSERT(it != end());
            return it->second;
        }

        template<typename KeyLikeT>
        [[nodiscard]] constexpr bool contains(const KeyLikeT &key) const noexcept
            requires details::TEqualTo<CompareT, KeyT, KeyLikeT> {
            return find(key) != end();
        }

        // mutation (write-rare path)

        template<typename... ArgsT>
            requires std::is_constructible_v<internal_pair, ArgsT...>
        iterator emplace(internal_pair &&value) {
            return insert<internal_pair>(std::move(value));
        }

        std::pair<iterator, bool> insert(internal_pair &&value) {
            return insert<internal_pair>(std::move(value));
        }

        template<std::convertible_to<internal_pair> ValueTLike>
        std::pair<iterator, bool> insert(ValueTLike &&value_like) {
            const auto existing = findEytzinger_(value_like.first);
            if (existing < m_size) {
                return {iterator(this, existing), false};
            }

            reserve(m_size + 1u);

            internal_pair *const temp = allocator_type::template allocate<internal_pair>(m_size + 1u);
            mem::unpoisonUninitialized(temp, m_size + 1u);

            std::uninitialized_move_n(m_data, m_size, temp);
            std::construct_at(&temp[m_size], std::forward<ValueTLike>(value_like));

            std::sort(temp, temp + m_size + 1u,
                      [](const internal_pair &a, const internal_pair &b) {
                          return CompareT{}(a.first, b.first);
                      });

            if (m_size > 0u) {
                std::destroy_n(m_data, m_size);
            }

            buildEytzingerFromSorted_(temp, m_size + 1u);
            std::destroy_n(temp, m_size + 1u);
            allocator_type::deallocate(temp, m_size + 1u);
            ++m_size;

            const auto inserted_pos = findEytzinger_(value_like.first);
            PPR_ASSERT(inserted_pos < m_size);
            return {iterator(this, inserted_pos), true};
        }

        template<std::ranges::forward_range RangeT>
            requires std::is_convertible_v<std::ranges::range_value_t<RangeT>, internal_pair>
        void append(RangeT &&values) {
            const u32 n = checked_cast<u32>(std::ranges::distance(values));
            if (n == 0u) [[unlikely]] {
                return;
            }

            reserve(m_size + n);

            const u32 total = m_size + n;
            internal_pair *const temp = allocator_type::template allocate<internal_pair>(total);
            mem::unpoisonUninitialized(temp, total);

            u32 constructed = 0u;
            u32 unique_count = 0u;
            try {
                if (m_size > 0u) [[likely]] {
                    std::uninitialized_move_n(m_data, m_size, temp);
                    constructed = m_size;
                }

                u32 i = m_size;
                for (auto &&v: values) {
                    std::construct_at(&temp[i], std::forward<decltype(v)>(v));
                    ++i;
                }
                constructed = total;

                std::stable_sort(temp, temp + total,
                                 [](const internal_pair &a, const internal_pair &b) {
                                     return CompareT{}(a.first, b.first);
                                 });

                const auto eq = [](const internal_pair &a, const internal_pair &b) noexcept {
                    return not CompareT{}(a.first, b.first) and not CompareT{}(b.first, a.first);
                };
                const auto new_end = std::unique(temp, temp + total, eq);
                unique_count = checked_cast<u32>(new_end - temp);

                if (m_size > 0u) [[likely]] {
                    std::destroy_n(m_data, m_size);
                    mem::poisonDestroyed(m_data, m_size);
                }

                buildEytzingerFromSorted_(temp, unique_count);
                std::destroy_n(temp, unique_count);
                mem::poisonDestroyed(temp, unique_count);
                allocator_type::deallocate(temp, total);
                m_size = unique_count;
            } catch (...) {
                std::destroy_n(temp, constructed);
                mem::poisonDestroyed(temp, constructed);
                allocator_type::deallocate(temp, total);
                throw;
            }
        }

        template<std::forward_iterator Iter>
            requires std::is_convertible_v<typename std::iterator_traits<Iter>::value_type, internal_pair>
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
                mem::poisonDestroyed(&m_data[0]);
                m_size = 0u;
                return;
            }

            internal_pair *const temp = allocator_type::template allocate<internal_pair>(m_size - 1u);
            mem::unpoisonUninitialized(temp, m_size - 1u);

            std::uninitialized_move(m_data, m_data + erase_pos, temp);
            std::uninitialized_move(m_data + erase_pos + 1u, m_data + m_size, temp + erase_pos);

            std::destroy_n(m_data, m_size);
            mem::poisonDestroyed(m_data, m_size);

            std::sort(temp, temp + m_size - 1u,
                      [](const internal_pair &a, const internal_pair &b) {
                          return CompareT{}(a.first, b.first);
                      });
            buildEytzingerFromSorted_(temp, m_size - 1u);

            std::destroy_n(temp, m_size - 1u);
            mem::poisonDestroyed(temp, m_size - 1u);
            allocator_type::deallocate(temp, m_size - 1u);
            --m_size;
        }

        template<std::ranges::range RangeT>
        void assignAssumeEmpty(RangeT &&values)
            requires std::is_convertible_v<std::ranges::range_value_t<RangeT>, internal_pair> {
            const u32 n = checked_cast<u32>(std::ranges::size(values));
            reserveAssumeEmpty(n);
            PPR_ASSERT(m_capacity >= n);

            auto *const first = m_data;
            mem::unpoisonUninitialized(first, n);
            if constexpr (std::ranges::sized_range<RangeT> && std::ranges::contiguous_range<RangeT>) {
                std::uninitialized_copy(std::ranges::begin(values), std::ranges::end(values), first);
            } else {
                u32 i = 0u;
                for (auto &&v: values) {
                    std::construct_at(&first[i], std::forward<decltype(v)>(v));
                    ++i;
                }
            }

            m_size = n;

            std::sort(first, first + m_size,
                      [](const internal_pair &a, const internal_pair &b) {
                          return CompareT{}(a.first, b.first);
                      });

            if (m_size > 1u) {
                internal_pair *const temp = allocator_type::template allocate<internal_pair>(m_size);
                mem::unpoisonUninitialized(temp, m_size);
                std::uninitialized_move_n(first, m_size, temp);
                std::destroy_n(first, m_size);
                buildEytzingerFromSorted_(temp, m_size);
                std::destroy_n(temp, m_size);
                allocator_type::deallocate(temp, m_size);
            }
        }

        friend void swap(FlatMap &lhs, FlatMap &rhs) noexcept {
            using namespace std;

            swap(static_cast<allocator_type &>(lhs), static_cast<allocator_type &>(rhs));
            swap(lhs.m_data, rhs.m_data);
            swap(lhs.m_capacity, rhs.m_capacity);
            swap(lhs.m_size, rhs.m_size);
        }

    private:
        void rebuildFromSorted_() noexcept {
            if (m_size <= 1u) {
                return;
            }
            internal_pair *const temp = allocator_type::template allocate<internal_pair>(m_size);
            mem::unpoisonUninitialized(temp, m_size);
            std::uninitialized_move_n(m_data, m_size, temp);
            std::destroy_n(m_data, m_size);
            buildEytzingerFromSorted_(temp, m_size);
            std::destroy_n(temp, m_size);
            allocator_type::deallocate(temp, m_size);
        }

        void grow_(const u32 new_capacity) {
            PPR_ASSERT(new_capacity > m_capacity);

            internal_pair *const new_data = allocator_type::template allocate<internal_pair>(new_capacity);
            u32 constructed = 0u;
            try {
                if (m_size > 0u) [[likely]] {
                    if constexpr (pP::details::is_relocatable_v<internal_pair>) {
                        std::memcpy(new_data, m_data, sizeof(internal_pair) * m_size);
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

        void buildEytzingerFromSorted_(const internal_pair *sorted, const u32 n) noexcept {
            mem::unpoisonUninitialized(m_data, n);
            u32 cursor = 0u;
            buildEytzingerImpl_(sorted, n, 0u, cursor);
        }

        void buildEytzingerImpl_(const internal_pair *sorted, const u32 n, const u32 eytz_pos, u32 &cursor) noexcept {
            if (eytz_pos >= n)
                return;

            buildEytzingerImpl_(sorted, n, 2u * eytz_pos + 1u, cursor);

            if (cursor < n) {
                std::construct_at(&m_data[eytz_pos], std::move(const_cast<internal_pair &>(sorted[cursor])));
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
                if (CompareT{}(m_data[i].first, key)) {
                    i = 2u * i + 2u;
                } else if (CompareT{}(key, m_data[i].first)) {
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
                if (!CompareT{}(m_data[i].first, key)) {
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
                if (CompareT{}(key, m_data[i].first)) {
                    result = i;
                    i = 2u * i + 1u;
                } else {
                    i = 2u * i + 2u;
                }
            }
            return result;
        }

        void spliceAssumeEmpty(FlatMap &src) noexcept {
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

    template<std::input_iterator Iter>
    FlatMap(Iter, Iter)
        -> FlatMap<
            std::remove_cvref_t<decltype(std::declval<Iter>()->first)>,
            std::remove_cvref_t<decltype(std::declval<Iter>()->second)> >;

    // extern template declarations

    extern template class FlatMap<u32, u32>;
    extern template class FlatMap<u64, u64>;
    extern template class FlatMap<u32, float>;
}
