module;
#include "pP/Macros.h"
export module engine.core:hash_map;

import :assert;
import :containers;
import :memory;

import std;

export namespace pP {
    // ------------------------------------------------------------------
    // naive robin hood hash map with out-of-core metadata
    // ------------------------------------------------------------------

    template<typename KeyT, typename ValueT,
        details::TEqualTo<KeyT> EqualToT = std::equal_to<KeyT>,
        hash::THasher<KeyT> HasherT = hash::DefaultHash<KeyT>,
        mem::details::TAllocator AllocatorT = mem::GPA>
    class HashMap;

    template<typename KeyT,
        details::TEqualTo<KeyT> EqualToT = std::equal_to<KeyT>,
        hash::THasher<KeyT> HasherT = hash::DefaultHash<KeyT>,
        mem::details::TAllocator AllocatorT = mem::GPA>
    using HashSet = HashMap<KeyT, void, EqualToT, HasherT, AllocatorT>;

    namespace details {
        // ------------------------------------------------------------
        // alias for hash map (std::pair<K,V>) vs hash set (bare K)
        // ------------------------------------------------------------

        // Internal type uses non-const KeyT for swap/move operations in the hash map implementation.
        // The iterator uses reinterpret_cast to present pair<const K,V> to users.
        template<typename KeyT, typename ValueT>
        using hashmap_value_t = std::conditional_t<
            std::is_void_v<ValueT>,
            KeyT,
            std::pair<KeyT, ValueT> >;

        // const key to avoid client corrupting the table
        template<typename KeyT, typename ValueT = void>
        struct hashmap_reference {
            using type = std::pair<const KeyT &, ValueT &>;
        };

        template<typename KeyT>
        struct hashmap_reference<KeyT, void> {
            using type = const KeyT &;
        };

        template<typename KeyT>
        struct hashmap_reference<KeyT, const void> {
            using type = const KeyT &;
        };

        template<typename KeyT, typename ValueT>
        using hashmap_reference_t = hashmap_reference<KeyT, ValueT>::type;

        // ------------------------------------------------------------
        // naive hash map iterator, expect bad iteration times
        // ------------------------------------------------------------

        template<typename KeyT, typename ValueT,
            TEqualTo<KeyT> EqualToT,
            hash::THasher<KeyT> HasherT,
            mem::details::TAllocator AllocatorT>
        class HashMapIterator {
            using hashmap_type = HashMap<
                KeyT,
                std::remove_const_t<ValueT>,
                EqualToT, HasherT, AllocatorT>;
            friend hashmap_type;

            friend class HashMapIterator<
                KeyT,
                std::add_const_t<ValueT>,
                EqualToT, HasherT, AllocatorT>;

        public:
            using hashmap_pointer = std::add_pointer_t<std::conditional_t<
                std::is_const_v<ValueT>,
                std::add_const_t<hashmap_type>,
                hashmap_type
            > >;

            using value_type = hashmap_value_t<KeyT, ValueT>;
            using reference = hashmap_reference_t<KeyT, ValueT>;

            using iterator_category = std::bidirectional_iterator_tag;
            using iterator_concept = std::bidirectional_iterator_tag; // C++20+

            using difference_type = std::ptrdiff_t;

        private:
            class ArrowProxy_ {
                reference m_ref;

                friend HashMapIterator;

                explicit constexpr ArrowProxy_(const HashMapIterator &iter) noexcept
                    : m_ref(*iter) {
                }

            public:
                [[nodiscard]] constexpr const reference *operator->() const noexcept {
                    return std::addressof(m_ref);
                }
            };

            constexpr void initFromIndex_() noexcept {
                if (!m_hash_table || m_slot >= m_hash_table->capacity()) [[unlikely]] {
                    m_slot = umax_v;
                }
            }

            hashmap_pointer m_hash_table{nullptr};

            u32 m_slot{umax_v}; // umax_v means end()

        public:
            using pointer = ArrowProxy_;

            constexpr HashMapIterator() noexcept = default;

            constexpr HashMapIterator(const HashMapIterator &) noexcept = default;

            constexpr HashMapIterator &operator =(const HashMapIterator &) noexcept = default;

            constexpr HashMapIterator(HashMapIterator &&) noexcept = default;

            constexpr HashMapIterator &operator =(HashMapIterator &&) noexcept = default;

            constexpr HashMapIterator(hashmap_pointer p_map PPR_LIFETIME_BOUND, const u32 slot_maybe_invalid) noexcept
                : m_hash_table(p_map), m_slot(slot_maybe_invalid) {
                initFromIndex_();
            }

            constexpr HashMapIterator(std::in_place_t, hashmap_pointer p_map PPR_LIFETIME_BOUND, const u32 slot) noexcept
                : m_hash_table(p_map), m_slot(slot) {
            }

            constexpr HashMapIterator(hashmap_pointer p_map PPR_LIFETIME_BOUND, const UnsignedMax end) noexcept
                : m_hash_table(p_map), m_slot(end) {
            }

            using non_const_iterator = HashMapIterator<
                KeyT,
                std::remove_const_t<ValueT>,
                EqualToT, HasherT, AllocatorT>;

            explicit constexpr HashMapIterator(const non_const_iterator &other) noexcept
                requires std::is_const_v<ValueT>
                : m_hash_table(other.m_hash_table), m_slot(other.m_slot) {
            }

            constexpr HashMapIterator &operator =(const non_const_iterator &other) noexcept
                requires std::is_const_v<ValueT> {
                m_hash_table = other.m_hash_table;
                m_slot = other.m_slot;
                return *this;
            }

            [[nodiscard]] constexpr bool isValid() const noexcept {
                return m_hash_table != nullptr &&
                       m_slot < m_hash_table->capacity();
            }

            // ------------------------------------------------------------
            // dereference
            // ------------------------------------------------------------
            [[nodiscard]] reference operator*() const noexcept {
                PPR_ASSERT(isValid());
                return reference(m_hash_table->m_values[m_slot]);
            }

            [[nodiscard]] pointer operator->() const noexcept {
                return ArrowProxy_(*this);
            }

            // ------------------------------------------------------------
            // increment
            // ------------------------------------------------------------
            constexpr HashMapIterator &operator++() noexcept {
                PPR_ASSERT(isValid());

                for (;;) {
                    if (++m_slot == m_hash_table->m_capacity_pow2_m1 + 1u) [[unlikely]] {
                        m_slot = umax_v;
                        break;
                    }
                    if (m_hash_table->m_metadata[m_slot].m_psl != 0u) [[likely]] {
                        break;
                    }
                }

                return *this;
            }

            constexpr HashMapIterator operator++(int) noexcept {
                auto tmp = *this;
                ++(*this);
                return tmp;
            }

            // ------------------------------------------------------------
            // decrement
            // ------------------------------------------------------------
            constexpr HashMapIterator &operator--() noexcept {
                PPR_ASSERT(isValid());

                for (;;) {
                    if (m_slot-- == 0u) [[unlikely]] {
                        m_slot = umax_v;
                        break;
                    }
                    if (m_hash_table->m_metadata[m_slot].m_psl != 0u) [[likely]] {
                        break;
                    }
                }

                return *this;
            }

            constexpr HashMapIterator operator--(int) noexcept {
                auto tmp = *this;
                --(*this);
                return tmp;
            }

            // ------------------------------------------------------------
            // comparisons
            // ------------------------------------------------------------
            [[nodiscard]] friend constexpr bool
            operator==(const HashMapIterator &a, const HashMapIterator &b) noexcept {
                PPR_ASSERT(a.m_hash_table == b.m_hash_table);
                return a.m_slot == b.m_slot;
            }

            [[nodiscard]] friend constexpr std::strong_ordering
            operator<=>(const HashMapIterator &a, const HashMapIterator &b) noexcept {
                PPR_ASSERT(a.m_hash_table == b.m_hash_table);
                return a.m_slot <=> b.m_slot;
            }
        };
    }

    // ------------------------------------------------------------
    // CTAD deduction guides for initializer_list and iterator-pair construction
    // ------------------------------------------------------------

    template<typename KeyT, typename ValueT,
        details::TEqualTo<KeyT> EqualToT = std::equal_to<KeyT>,
        hash::THasher<KeyT> HasherT = hash::DefaultHash<KeyT>,
        mem::details::TAllocator AllocatorT = mem::GPA>
    HashMap(std::initializer_list<std::pair<KeyT, ValueT> >)
        -> HashMap<KeyT, ValueT, EqualToT, HasherT, AllocatorT>;

    template<typename KeyT,
        details::TEqualTo<KeyT> EqualToT = std::equal_to<KeyT>,
        hash::THasher<KeyT> HasherT = hash::DefaultHash<KeyT>,
        mem::details::TAllocator AllocatorT = mem::GPA>
    HashMap(std::initializer_list<KeyT>)
        -> HashSet<KeyT, EqualToT, HasherT, AllocatorT>;

    template<typename KeyT, typename ValueT,
        details::TEqualTo<KeyT> EqualToT = std::equal_to<KeyT>,
        hash::THasher<KeyT> HasherT = hash::DefaultHash<KeyT>,
        mem::details::TAllocator AllocatorT = mem::GPA>
    HashMap(std::initializer_list<std::pair<KeyT, ValueT> >, const AllocatorT &)
        -> HashMap<KeyT, ValueT, EqualToT, HasherT, AllocatorT>;

    template<typename KeyT,
        details::TEqualTo<KeyT> EqualToT = std::equal_to<KeyT>,
        hash::THasher<KeyT> HasherT = hash::DefaultHash<KeyT>,
        mem::details::TAllocator AllocatorT = mem::GPA>
    HashMap(std::initializer_list<KeyT>, const AllocatorT &)
        -> HashSet<KeyT, EqualToT, HasherT, AllocatorT>;

    template<std::input_iterator Iter>
    HashMap(Iter, Iter)
        -> HashMap<
            std::remove_cvref_t<decltype(std::declval<Iter>()->first)>,
            std::remove_cvref_t<decltype(std::declval<Iter>()->second)> >;

    template<std::input_iterator Iter>
    HashMap(Iter, Iter, mem::GPA)
        -> HashMap<
            std::remove_cvref_t<decltype(std::declval<Iter>()->first)>,
            std::remove_cvref_t<decltype(std::declval<Iter>()->second)> >;

    // ------------------------------------------------------------
    // Round robin hash map/set implementation (ValueT==void -> HashSet)
    // ------------------------------------------------------------

    template<typename KeyT, typename ValueT,
        details::TEqualTo<KeyT> EqualToT,
        hash::THasher<KeyT> HasherT,
        mem::details::TAllocator AllocatorT>
    class HashMap : mem::Allocator<AllocatorT> {
        using allocator_type = mem::Allocator<AllocatorT>;
        using key_equal = EqualToT;
        using hasher = HasherT;

    public:
        using value_type = details::hashmap_value_t<KeyT, ValueT>;
        using reference = details::hashmap_reference_t<KeyT, ValueT>;
        using const_reference = details::hashmap_reference_t<KeyT, const ValueT>;
        using size_type = std::size_t;

        using iterator = details::HashMapIterator<KeyT, std::remove_const_t<ValueT>, EqualToT, HasherT, AllocatorT>;
        using const_iterator = details::HashMapIterator<KeyT, std::add_const_t<ValueT>, EqualToT, HasherT, AllocatorT>;

        static_assert(std::bidirectional_iterator<iterator>);
        static_assert(std::bidirectional_iterator<const_iterator>);

        friend iterator;
        friend const_iterator;

    private:
        static constexpr std::size_t min_capacity_v = 4u;

        static constexpr u8 bits_h1_v = 3u;
        static constexpr u8 bits_psl_v = 5u;

        static constexpr u8 mask_h1_v = (1u << bits_h1_v) - 1u;
        static constexpr u8 mask_psl_v = (1u << bits_psl_v) - 1u;

        struct Location {
            u32 m_slot: (32 - bits_h1_v) = 0u;
            u32 m_h1: bits_h1_v = 0u;
        };

        struct Metadata {
            u8 m_h1: bits_h1_v = 0u;
            u8 m_psl: bits_psl_v = 0u;
        };

        template<typename KeyTLike>
        [[nodiscard]] PPR_FORCE_INLINE constexpr Location
        keyLocation_(const KeyTLike &key_like) const noexcept
            requires hash::THasher<HasherT, KeyTLike> {
            const hash_t H = hash::combine(hash_t{std::bit_cast<std::uintptr_t>(m_values)}, hasher{}(key_like));
            return Location{
                .m_slot = static_cast<u32>(H.m_value >> bits_h1_v) & m_capacity_pow2_m1,
                .m_h1 = static_cast<u8>(H.m_value & mask_h1_v),
            };
        }

        [[nodiscard]] PPR_FORCE_INLINE constexpr AllocatorT &getAllocator_() noexcept {
            return allocator_type::materialize();
        }

        [[nodiscard]] PPR_FORCE_INLINE constexpr const AllocatorT &getAllocator_() const noexcept {
            return allocator_type::materialize();
        }

        template<std::convertible_to<value_type> ValueTLike>
        [[nodiscard]] PPR_FORCE_INLINE static constexpr const KeyT &
        key_(const ValueTLike &value_like) noexcept {
            if constexpr (std::is_void_v<ValueT>) {
                return value_like;
            } else {
                return value_like.first;
            }
        }

        template<typename KeyTLike>
        [[nodiscard]] PPR_FORCE_INLINE static constexpr bool
        keyEqual_(const KeyT &lhs, const KeyTLike &rhs) noexcept
            requires details::TEqualTo<EqualToT, KeyT, KeyTLike> {
            return key_equal{}(lhs, rhs);
        }

        // Slot that key exists in or should exist in, true means found, false not found.
        template<typename KeyTLike>
        [[nodiscard]] PPR_FLATTEN constexpr std::pair<u32, bool>
        findSlot_(const KeyTLike &key_like, const Location &loc) const noexcept
            requires details::TEqualTo<EqualToT, KeyT, KeyTLike> && hash::THasher<HasherT, KeyTLike> {
            for (u32 psl = 0u; psl < m_capacity_pow2_m1;) {
                const u32 offset = (loc.m_slot + psl) & m_capacity_pow2_m1;
                const u32 table_psl = m_metadata[offset].m_psl;

                ++psl; // bias psl by one, convenient to do it here
                PPR_ASSERT((psl & mask_psl_v) == psl);

                if (table_psl > psl) {
                    // Definitely not, continue.
                    continue;
                }

                if (table_psl < psl) {
                    // definite miss.
                    return {offset, false};
                }

                if (table_psl == psl) {
                    // maybe
                    if (m_metadata[offset].m_h1 == loc.m_h1 &&
                        keyEqual_(key_(m_values[offset]), key_like)) {
                        return {offset, true};
                    }
                }
            }

            return {umax_v, false};
        }

        value_type *m_values{nullptr};
        Metadata *m_metadata{nullptr};

        u32 m_capacity_pow2_m1{0u};
        u32 m_size{0u};

    public:
        constexpr HashMap() noexcept(std::is_nothrow_default_constructible_v<allocator_type>)
            requires std::is_default_constructible_v<allocator_type> = default;

        explicit constexpr HashMap(const AllocatorT &al)
            noexcept(std::is_nothrow_copy_constructible_v<allocator_type>)
            requires std::is_copy_constructible_v<allocator_type>
            : allocator_type(al) {
        }

        explicit constexpr HashMap(AllocatorT &&al)
            noexcept(std::is_nothrow_move_constructible_v<allocator_type>)
            requires std::is_move_constructible_v<allocator_type>
            : allocator_type(std::move(al)) {
        }

        constexpr HashMap(const std::size_t initial_capacity)
            requires std::is_default_constructible_v<allocator_type> {
            reserveAssumeEmpty(initial_capacity);
        }

        explicit constexpr HashMap(const std::size_t initial_capacity, const AllocatorT &al)
            requires std::is_copy_constructible_v<allocator_type>
            : allocator_type(al) {
            reserveAssumeEmpty(initial_capacity);
        }

        explicit constexpr HashMap(const std::size_t initial_capacity, AllocatorT &&al)
            requires std::is_move_constructible_v<allocator_type>
            : allocator_type(std::move(al)) {
            reserveAssumeEmpty(initial_capacity);
        }

        constexpr HashMap(const std::initializer_list<value_type> initial_values)
            requires std::is_default_constructible_v<allocator_type> {
            assignAssumeEmpty(initial_values);
        }

        explicit constexpr HashMap(const std::initializer_list<value_type> initial_values, const AllocatorT &al)
            requires std::is_copy_constructible_v<allocator_type>
            : allocator_type(al) {
            assignAssumeEmpty(initial_values);
        }

        explicit constexpr HashMap(const std::initializer_list<value_type> initial_values, AllocatorT &&al)
            requires std::is_move_constructible_v<allocator_type>
            : allocator_type(std::move(al)) {
            assignAssumeEmpty(initial_values);
        }

        constexpr HashMap(const HashMap &other)
            : allocator_type(other.getAllocator_()) {
            assignAssumeEmpty(other);
        }

        constexpr HashMap &operator =(const HashMap &other) {
            if (this == &other) [[unlikely]] {
                return *this;
            }

            reset();
            allocator_type::operator =(other.getAllocator_());
            assignAssumeEmpty(other);
            return *this;
        }

        constexpr HashMap(HashMap &&other) noexcept
            : allocator_type(std::move(other)) {
            spliceAssumeEmpty(other);
        }

        constexpr HashMap &operator =(HashMap &&other) noexcept {
            if (this == &other) [[unlikely]] {
                return *this;
            }

            reset();
            allocator_type::operator =(std::move(other));
            spliceAssumeEmpty(other);
            return *this;
        }

        constexpr ~HashMap() noexcept {
            reset();
        }

        [[nodiscard]] PPR_FORCE_INLINE constexpr std::size_t capacity() const noexcept {
            return m_capacity_pow2_m1 ? m_capacity_pow2_m1 + 1u : 0u;
        }

        [[nodiscard]] PPR_FORCE_INLINE constexpr std::size_t size() const noexcept {
            return m_size;
        }

        [[nodiscard]] PPR_FORCE_INLINE constexpr bool isEmpty() const noexcept {
            return m_size == 0u;
        }

        [[nodiscard]] float getLoadFactor() const noexcept {
            return m_capacity_pow2_m1 ? static_cast<float>(m_size) / (1u + m_capacity_pow2_m1) : 0u;
        }

        [[nodiscard]] constexpr iterator begin() noexcept {
            if (m_capacity_pow2_m1 == 0u) {
                return end();
            }
            iterator first(std::in_place_t{}, this, 0u);
            if (m_metadata[0u].m_psl == 0u) {
                ++first;
            }
            return first;
        }

        [[nodiscard]] constexpr iterator end() noexcept {
            return iterator(this, umax_v);
        }

        [[nodiscard]] constexpr const_iterator begin() const noexcept {
            return const_iterator(const_cast<HashMap *>(this)->begin());
        }

        [[nodiscard]] constexpr const_iterator end() const noexcept {
            return const_iterator(this, umax_v);
        }

        [[nodiscard]] constexpr const_iterator cbegin() const noexcept {
            return begin();
        }

        [[nodiscard]] constexpr const_iterator cend() const noexcept {
            return end();
        }

        [[nodiscard]] constexpr auto keys(this auto &&self) noexcept
            requires (!std::is_void_v<ValueT>) {
            return self | std::views::transform([](const value_type &value) -> const KeyT & {
                return value.first;
            });
        }

        [[nodiscard]] constexpr auto values(this auto &&self) noexcept
            requires (!std::is_void_v<ValueT>) {
            return self | std::views::transform([](const value_type &value) -> const ValueT & {
                return value.second;
            });
        }

        template<std::ranges::range RangeT>
        void assign(RangeT &&values)
            noexcept(std::is_nothrow_copy_constructible_v<value_type>)
            requires std::is_convertible_v<std::ranges::range_value_t<RangeT>, value_type> {
            clear();
            assignAssumeEmpty(std::forward<RangeT>(values));
        }

        template<std::ranges::range RangeT>
        void assignAssumeEmpty(RangeT &&values)
            noexcept(std::is_nothrow_copy_constructible_v<value_type>)
            requires std::is_convertible_v<std::ranges::range_value_t<RangeT>, value_type> {
            const std::size_t n = std::ranges::size(values);
            reserveAssumeEmpty(n);
            appendAssumeCapacity(std::forward<RangeT>(values));
        }

        void append(std::initializer_list<value_type> initializer_list)
            noexcept(std::is_nothrow_copy_constructible_v<value_type>) {
            reserveAdditional(initializer_list.size());
            appendAssumeCapacity(initializer_list);
        }

        template<std::ranges::range RangeT>
        void append(RangeT &&values)
            noexcept(std::is_nothrow_copy_constructible_v<value_type>)
            requires std::is_convertible_v<std::ranges::range_value_t<RangeT>, value_type> {
            const std::size_t n = std::ranges::size(values);
            reserveAdditional(n);
            appendAssumeCapacity(std::forward<RangeT>(values));
        }

        template<std::ranges::range RangeT>
        void appendAssumeCapacity(RangeT &&values)
            noexcept(std::is_nothrow_copy_constructible_v<value_type>)
            requires std::is_convertible_v<std::ranges::range_value_t<RangeT>, value_type> {
            for (auto &&value: values) {
                insertAssumeCapacity(value);
            }
        }

        PPR_FORCE_INLINE std::pair<iterator, bool> insert(value_type &&value) noexcept {
            reserveAdditional(1u);
            return insertAssumeCapacity(std::move(value));
        }

        template<std::convertible_to<value_type> ValueTLike>
        std::pair<iterator, bool> insert(ValueTLike &&value_like) noexcept {
            reserveAdditional(1u);
            return insertAssumeCapacity(std::forward<ValueTLike>(value_like));
        }

        template<std::convertible_to<value_type> ValueTLike>
        std::pair<iterator, bool> insertAssumeCapacity(ValueTLike &&value_like) noexcept {
            PPR_ASSERT(m_size + 1u/* terminator */ < m_capacity_pow2_m1 + 1u);

            const KeyT &key = key_(value_like);
            Location loc = keyLocation_(key);
            auto [offset, found] = findSlot_(key, loc);

            if (found) {
                // already in
                return std::make_pair(iterator(this, offset), false);
            }

            ++m_size;
            value_type carry_value{std::forward<ValueTLike>(value_like)};

            // eviction loop
            const u32 inserted_at = offset;
            PPR_EXPR_IF_DEBUG(const Location loc_for_debug = loc);

            for (;; ++offset) {
                const u32 slot = offset & m_capacity_pow2_m1;
                const u32 psl = (offset + m_capacity_pow2_m1 + 2u - loc.m_slot) & m_capacity_pow2_m1;
                PPR_ASSERT((psl & mask_psl_v) == psl && "psl overflow, table is too full!");

                const u32 table_psl = m_metadata[slot].m_psl;
                if (table_psl == 0u) [[likely]] {
                    // empty slot, insert here
                    std::construct_at(&m_values[slot], std::move(carry_value));
                    m_metadata[slot] = Metadata{.m_h1 = static_cast<u8>(loc.m_h1), .m_psl = static_cast<u8>(psl)};

                    PPR_EXPR_IF_DEBUG(PPR_ASSERT(loc_for_debug.m_h1 == m_metadata[inserted_at].m_h1));
                    PPR_EXPR_IF_DEBUG(PPR_ASSERT(loc_for_debug.m_slot == (
                        (inserted_at + m_capacity_pow2_m1 + 2u - m_metadata[inserted_at].m_psl) & m_capacity_pow2_m1)));
                    return std::make_pair(iterator(std::in_place_t{}, this, inserted_at), true);
                }

                // evicting insert: insert this key, cycle insert next key
                if (table_psl < psl) {
                    const u8 old_h1 = m_metadata[slot].m_h1;
                    std::swap(m_values[slot], carry_value);

                    m_metadata[slot] = Metadata{.m_h1 = static_cast<u8>(loc.m_h1), .m_psl = static_cast<u8>(psl)};

                    loc.m_h1 = old_h1;
                    loc.m_slot = (offset + m_capacity_pow2_m1 + 2u - table_psl) & m_capacity_pow2_m1;
                }
            }
        }

        template<typename KeyTLike>
        [[nodiscard]] auto find(this auto &&self, KeyTLike &&key_like) noexcept
            requires details::TEqualTo<EqualToT, KeyT, KeyTLike> && hash::THasher<HasherT, KeyTLike> {
            const Location loc = self.keyLocation_(key_like);
            const auto [offset, found] = self.findSlot_(std::forward<KeyTLike>(key_like), loc);
            auto it = self.end();
            if (found) [[likely]] {
                it.m_slot = offset;
            }
            return it;
        }

        template<typename KeyTLike>
        [[nodiscard]] auto &operator[](this auto &&self, KeyTLike &&key_like) noexcept
            requires (!std::is_void_v<ValueT> &&
                      details::TEqualTo<EqualToT, KeyT, KeyTLike> &&
                      hash::THasher<HasherT, KeyTLike>) {
            const auto it = self.find(std::forward<KeyTLike>(key_like));
            PPR_ASSERT(self.end() != it);
            return it->second;
        }

        template<typename KeyTLike>
        bool erase(const KeyTLike &key) noexcept
            requires details::TEqualTo<EqualToT, KeyT, KeyTLike> && hash::THasher<HasherT, KeyTLike> {
            if (const auto [slot, found] = findSlot_(key, keyLocation_(key)); found) {
                eraseAt(const_iterator{this, slot});
                return true;
            }
            return false;
        }

        void eraseAt(const const_iterator it) noexcept {
            PPR_ASSERT(m_size > 0u && m_capacity_pow2_m1 > 0u);
            PPR_ASSERT(it.m_hash_table == this && it.m_slot < m_capacity_pow2_m1 + 1u);

            // eviction loop
            u32 slot = it.m_slot;
            u32 prev;
            for (;;) {
                prev = slot;
                slot = (slot + 1u) & m_capacity_pow2_m1;

                const Metadata meta = m_metadata[slot];
                if (meta.m_psl <= 1u) {
                    break;
                }

                PPR_ASSERT(m_metadata[prev].m_psl);
                m_values[prev] = std::move(m_values[slot]);
                m_metadata[prev] = Metadata{.m_h1 = meta.m_h1, .m_psl = static_cast<u8>(meta.m_psl - 1u)};
            }

            --m_size;
            m_metadata[prev] = default_value_v;
            std::destroy_at(&m_values[prev]);
            mem::poisonIfDebug(mem::Poison::destroyed, &m_values[prev]);
        }

        PPR_FORCE_INLINE void reserveAdditional(const std::size_t n) {
            reserve(m_size + n);
        }

        PPR_FORCE_INLINE void reserve(const std::size_t wanted_capacity) {
            if (wanted_capacity + 1u/* terminator */ > m_capacity_pow2_m1 + 1u) [[unlikely]] {
                if (m_size > 0u) [[likely]] {
                    rehash(std::max(static_cast<std::size_t>(m_capacity_pow2_m1 * 2u), wanted_capacity + 1u));
                } else {
                    reserveAssumeEmpty(wanted_capacity);
                }
            }
        }

        void reserveAssumeEmpty(std::size_t new_capacity) {
            new_capacity = std::bit_ceil(std::max(new_capacity + 1u/* terminator */, min_capacity_v));
            const u32 old_capacity = m_capacity_pow2_m1 ? m_capacity_pow2_m1 + 1u : 0u;
            PPR_ASSERT(m_size == 0u && (m_values == nullptr) == (m_metadata == nullptr));

            if (new_capacity == old_capacity) [[unlikely]] {
                return;
            }

            if (m_metadata != nullptr) [[unlikely]] {
                PPR_ASSERT(m_values != nullptr && old_capacity > 0u);
                allocator_type::deallocate(m_metadata, old_capacity);
                allocator_type::deallocate(m_values, old_capacity);
            }

            m_capacity_pow2_m1 = checked_cast<u32>(new_capacity - 1u);
            m_metadata = allocator_type::template allocate<Metadata>(new_capacity);
            m_values = allocator_type::template allocate<value_type>(new_capacity);

            std::memset(m_metadata, 0u, sizeof(Metadata) * new_capacity);
        }

        void rehash(std::size_t new_capacity) noexcept {
            new_capacity = std::bit_ceil(std::max(new_capacity + 1u/* terminator */, min_capacity_v));
            const u32 old_capacity = m_capacity_pow2_m1 ? m_capacity_pow2_m1 + 1u : 0u;
            if (old_capacity == new_capacity || new_capacity <= m_size) [[unlikely]] {
                return;
            }

            [[maybe_unused]] const u32 old_size = m_size;
            Metadata *const old_metadata = m_metadata;
            value_type *const old_values = m_values;

            PPR_DEFER {
                if (old_metadata != nullptr) [[unlikely]] {
                    PPR_ASSERT(old_values != nullptr && old_capacity > 0u);
                    allocator_type::deallocate(old_metadata, old_capacity);
                    allocator_type::deallocate(old_values, old_capacity);
                }
            };

            m_capacity_pow2_m1 = checked_cast<u32>(new_capacity - 1u);
            m_metadata = allocator_type::template allocate<Metadata>(new_capacity);
            m_values = allocator_type::template allocate<value_type>(new_capacity);
            m_size = 0u;

            std::memset(m_metadata, 0u, sizeof(Metadata) * new_capacity);

            for (u32 slot = 0u; slot < old_capacity; ++slot) {
                if (old_metadata[slot].m_psl) {
                    insertAssumeCapacity(std::move(old_values[slot]));
                    std::destroy_at(&old_values[slot]);
                }
            }
            PPR_ASSERT(m_size == old_size);
        }

        void clear() noexcept(std::is_nothrow_destructible_v<value_type>) {
            if (m_size == 0u) [[unlikely]] {
                return;
            }

            PPR_ASSERT(m_capacity_pow2_m1);
            const u32 capacity = m_capacity_pow2_m1 + 1u;

            if constexpr (!std::is_trivially_destructible_v<value_type>) {
                PPR_EXPR_IF_DEBUG(u32 size_for_debug = 0u);
                for (u32 i = 0u; i < capacity; ++i) {
                    if (m_metadata[i].m_psl) {
                        m_metadata[i] = default_value_v;
                        std::destroy_at(&m_values[i]);
                        PPR_EXPR_IF_DEBUG(++size_for_debug);
                        mem::poisonIfDebug(mem::Poison::destroyed, &m_values[i]);
                    }
                }
                PPR_EXPR_IF_DEBUG(PPR_ASSERT(size_for_debug == m_size));
            } else {
                std::memset(m_metadata, 0u, sizeof(Metadata) * capacity);
                mem::poisonIfDebug(mem::Poison::destroyed, m_values, capacity);
            }

            m_size = 0u;
        }

        void reset() {
            if (m_capacity_pow2_m1 == 0u) {
                PPR_ASSERT(m_values == nullptr && m_metadata == nullptr && m_size == 0u);
                return;
            }
            clear();

            PPR_ASSERT(m_size == 0u);
            const u32 capacity = m_capacity_pow2_m1 + 1u;
            allocator_type::deallocate(m_metadata, capacity);
            allocator_type::deallocate(m_values, capacity);

            m_metadata = nullptr;
            m_values = nullptr;
            m_capacity_pow2_m1 = 0u;
            m_size = 0u;
        }

        constexpr void spliceAssumeEmpty(HashMap &src) noexcept {
            PPR_ASSERT(m_values == nullptr && m_metadata == nullptr);

            m_values = src.m_values;
            m_metadata = src.m_metadata;
            m_capacity_pow2_m1 = src.m_capacity_pow2_m1;
            m_size = src.m_size;

            src.m_values = nullptr;
            src.m_metadata = nullptr;
            src.m_capacity_pow2_m1 = 0u;
            src.m_size = 0u;
        }

        constexpr friend void swap(HashMap &lhs, HashMap &rhs) noexcept {
            using namespace std;

            swap(static_cast<allocator_type &>(lhs), static_cast<allocator_type &>(rhs));

            swap(lhs.m_values, rhs.m_values);
            swap(lhs.m_metadata, rhs.m_metadata);
            swap(lhs.m_capacity_pow2_m1, rhs.m_capacity_pow2_m1);
            swap(lhs.m_size, rhs.m_size);
        }

        template<details::TEqualTo<KeyT> OtherEqualToT,
            hash::THasher<KeyT> OtherHasherT,
            mem::details::TAllocator OtherAllocatorT>
        [[nodiscard]] constexpr friend bool operator==(const HashMap &lhs, const HashMap<KeyT, ValueT, OtherEqualToT, OtherHasherT, OtherAllocatorT> &rhs) noexcept
            requires std::is_void_v<ValueT> || std::equality_comparable<ValueT> {
            if (lhs.size() != rhs.size()) [[likely]] {
                return false;
            }
            for (const auto &value: lhs) {
                const auto it = rhs.find(key_(value));
                if (it == rhs.end()) {
                    return false;
                }
                if constexpr (!std::is_void_v<ValueT>) {
                    if (!(it->second == value.second)) {
                        return false;
                    }
                }
            }
            return true;
        }

        [[nodiscard]] constexpr friend hash_t hashValue(const HashMap &hash_map) noexcept
            requires hash::THashable<value_type> {
            return hash::unorderedRange(hash_map);
        }
    };

    // ------------------------------------------------------------
    // HashMap are relocatable -memcopyable- if the allocator is relocatable
    // ------------------------------------------------------------

    template<typename KeyT, typename ValueT,
        details::TEqualTo<KeyT> EqualToT,
        hash::THasher<KeyT> HasherT,
        mem::details::TAllocator AllocatorT>
    struct details::relocatable<HashMap<KeyT, ValueT, EqualToT, HasherT, AllocatorT> > :
            relocatable<mem::Allocator<AllocatorT> > {
    };
}
