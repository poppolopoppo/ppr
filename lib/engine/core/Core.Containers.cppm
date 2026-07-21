module;
#include "pP/Macros.h"
export module engine.core:containers;

import :assert;
import :hal;
import :hashing;
import :memory.poison;

import std;

export namespace pP {
    namespace details {
        template<std::forward_iterator IteratorT, typename T>
        inline constexpr bool is_iterator_of = std::is_convertible_v<std::iter_value_t<IteratorT>, T>;

        template<typename EqualToT, typename LhsT, typename RhsT = LhsT>
        concept TEqualTo = requires(const std::remove_cvref_t<EqualToT> &cmp, const std::remove_cvref_t<LhsT> &lhs, const std::remove_cvref_t<RhsT> &rhs)
        {
            { cmp(lhs, rhs) } -> std::convertible_to<bool>;
        };
    }

    // ------------------------------------------------------------------
    // relocatable objects can be safely mem-copied instead of moving them
    // ------------------------------------------------------------------

    namespace details {
        template<typename T>
        struct relocatable : std::conjunction<
                    std::is_trivially_copyable<T>,
                    std::disjunction<std::is_fundamental<T>, std::is_pointer<T>, std::is_array<T> > > {
        };

        template<typename T>
        inline constexpr bool is_relocatable_v = relocatable<T>::value;
    }

    template<typename T> requires (details::is_relocatable_v<T> or std::is_move_constructible_v<T>)
    [[nodiscard]] constexpr T relocate(T *const src)
        noexcept(details::is_relocatable_v<T> or std::is_nothrow_move_constructible_v<T>) {
        PPR_ASSERT(src != nullptr);
        if constexpr (details::is_relocatable_v<T>) {
            alignas(T) T result{};
            std::memcpy(&result, src, sizeof(T));
            return result;
        } else {
            alignas(T) T result{std::move(*src)};
            std::destroy_at(src);
            return result;
        }
    }

    template<typename T> requires (details::is_relocatable_v<T> or std::is_move_constructible_v<T>)
    constexpr void relocateUninitialized(T *const src, T *const uninitialized)
        noexcept(details::is_relocatable_v<T> or std::is_nothrow_move_constructible_v<T>) {
        PPR_ASSERT(src != nullptr && uninitialized != nullptr);
        if constexpr (details::is_relocatable_v<T>) {
            std::memcpy(uninitialized, src, sizeof(T));
        } else {
            std::construct_at(uninitialized, std::move(*src));
        }
    }

    // ------------------------------------------------------------------
    // collector for generic push back container abstraction
    // ------------------------------------------------------------------

    template<typename T>
    struct Collector : std23::function_ref<std::error_code (const T &push_back)> {
        using super_t = std23::function_ref<std::error_code (const T &push_back)>;
        using super_t::super_t;
        using super_t::operator=;
        using super_t::operator();

        template<std::input_iterator IteratorT>
            requires std::convertible_to<typename std::iterator_traits<IteratorT>::reference, const T &>
        [[nodiscard]] std::error_code append(const IteratorT first, const IteratorT last) const {
            for (IteratorT it = first; it != last; ++it) {
                if (const std::error_code err = operator()(*it)) [[unlikely]] {
                    return err;
                }
            }
            return default_value_v;
        }

        [[nodiscard]] std::error_code append(std::initializer_list<T> ilist) const {
            return append(ilist.begin(), ilist.end());
        }

        template<std::ranges::input_range RangeT>
            requires std::convertible_to<std::ranges::range_const_reference_t<RangeT>, const T &>
        [[nodiscard]] std::error_code append(const RangeT &input_range) const {
            return append(std::ranges::begin(input_range), std::ranges::end(input_range));
        }

        [[nodiscard]] std::error_code combine(std::initializer_list<std23::function_ref<std::error_code(Collector)>> enumerators) const {
            for (const auto enumerate : enumerators) {
                if (const std::error_code err = enumerate(*this)) [[unlikely]] {
                    return err;
                }
            }
            return default_value_v;
        }
    };

    // ------------------------------------------------------------------
    // general purpose index iterator with random access
    // ------------------------------------------------------------------

    template<typename ContainerT, typename T, std::integral IndexT = std::size_t>
        requires requires(ContainerT &arr, IndexT index)
        {
            { arr[index] } -> std::convertible_to<T>;
        }
    class IndexIterator {
    public:
        using iterator_category = std::random_access_iterator_tag;
        using iterator_concept = std::random_access_iterator_tag; // C++20+
        using difference_type = std::make_signed_t<IndexT>;
        using value_type = T;
        using reference = decltype(std::declval<ContainerT &>()[std::declval<IndexT>()]);

        static constexpr bool return_by_reference_v = std::is_reference_v<reference>;

    private:
        ContainerT *m_container{nullptr};
        IndexT m_index{0};

    public:
        // ------------------------------------------------------------
        // constructors
        // ------------------------------------------------------------
        constexpr IndexIterator() noexcept = default;

        constexpr IndexIterator(ContainerT &container PPR_LIFETIME_BOUND, const IndexT index) noexcept
            : m_container(std::addressof(container)), m_index(index) {
        }

        // allow conversion to const iterator
        // ReSharper disable once CppNonExplicitConversionOperator
        constexpr operator IndexIterator<std::add_const_t<ContainerT>, std::add_const_t<T>, IndexT>() const noexcept {
            return {*m_container, m_index};
        }

        [[nodiscard]] constexpr ContainerT *getContainer() const noexcept {
            return m_container;
        }

        [[nodiscard]] constexpr IndexT getIndex() const noexcept {
            return m_index;
        }

        // ------------------------------------------------------------
        // dereference
        // ------------------------------------------------------------
        [[nodiscard]] constexpr reference operator*() const noexcept {
            PPR_ASSERT(m_container != nullptr);
            PPR_ASSUME(m_container != nullptr);
            return (*m_container)[m_index]; // use [] (not at) for iterator semantics
        }

        [[nodiscard]] constexpr std::add_pointer_t<value_type> operator->() const noexcept
            requires return_by_reference_v {
            return std::addressof(operator*());
        }

        [[nodiscard]] constexpr reference operator[](difference_type n) const noexcept {
            PPR_ASSERT(m_container != nullptr);
            PPR_ASSUME(m_container != nullptr);
            return (*m_container)[m_index + n];
        }

        // ------------------------------------------------------------
        // increment / decrement
        // ------------------------------------------------------------
        constexpr IndexIterator &operator++() noexcept {
            ++m_index;
            return *this;
        }

        constexpr IndexIterator operator++(int) noexcept {
            auto tmp = *this;
            ++*this;
            return tmp;
        }

        constexpr IndexIterator &operator--() noexcept {
            --m_index;
            return *this;
        }

        constexpr IndexIterator operator--(int) noexcept {
            auto tmp = *this;
            --*this;
            return tmp;
        }

        // ------------------------------------------------------------
        // arithmetic
        // ------------------------------------------------------------
        constexpr IndexIterator &operator+=(difference_type n) noexcept {
            m_index = checked_cast<IndexT>(static_cast<IndexT>(m_index) + static_cast<IndexT>(n));
            return *this;
        }

        constexpr IndexIterator &operator-=(difference_type n) noexcept {
            PPR_ASSERT(static_cast<difference_type>(m_index) >= n);
            m_index = checked_cast<IndexT>(static_cast<IndexT>(m_index) - static_cast<IndexT>(n));
            return *this;
        }

        [[nodiscard]] friend constexpr IndexIterator
        operator+(IndexIterator it, difference_type n) noexcept {
            it += n;
            return it;
        }

        [[nodiscard]] friend constexpr IndexIterator
        operator+(difference_type n, IndexIterator it) noexcept {
            it += n;
            return it;
        }

        [[nodiscard]] friend constexpr IndexIterator
        operator-(IndexIterator it, difference_type n) noexcept {
            it -= n;
            return it;
        }

        // ------------------------------------------------------------
        // comparisons (same type)
        // ------------------------------------------------------------
        [[nodiscard]] friend constexpr bool operator==(IndexIterator lhs, IndexIterator rhs) noexcept {
            PPR_ASSERT(lhs.m_container == rhs.m_container);
            return lhs.m_index == rhs.m_index;
        }

        [[nodiscard]] friend constexpr std::strong_ordering
        operator<=>(IndexIterator lhs, IndexIterator rhs) noexcept {
            PPR_ASSERT(lhs.m_container == rhs.m_container);
            return lhs.m_index <=> rhs.m_index;
        }

        // ------------------------------------------------------------
        // cross const comparisons
        // ------------------------------------------------------------
        [[nodiscard]] friend constexpr bool
        operator==(IndexIterator<std::add_const_t<ContainerT>, std::add_const_t<T>, IndexT> lhs,
                   IndexIterator rhs) noexcept
            requires (!std::is_same_v<std::add_const_t<T>, T>) {
            PPR_ASSERT(lhs.getContainer() == rhs.m_container);
            return lhs.getIndex() == rhs.m_index;
        }

        [[nodiscard]] friend constexpr std::strong_ordering
        operator<=>(IndexIterator<std::add_const_t<ContainerT>, std::add_const_t<T>, IndexT> lhs,
                    IndexIterator rhs) noexcept
            requires (!std::is_same_v<std::add_const_t<T>, T>) {
            PPR_ASSERT(lhs.getContainer() == rhs.m_container);
            return lhs.getIndex() <=> rhs.m_index;
        }

        // ------------------------------------------------------------
        // distance
        // ------------------------------------------------------------
        [[nodiscard]] friend constexpr difference_type
        operator-(IndexIterator lhs, IndexIterator rhs) noexcept {
            PPR_ASSERT(lhs.m_container == rhs.m_container);
            return static_cast<difference_type>(lhs.m_index)
                   - static_cast<difference_type>(rhs.m_index);
        }

        [[nodiscard]] friend constexpr difference_type
        operator-(IndexIterator<std::add_const_t<ContainerT>, std::add_const_t<T>, IndexT> lhs,
                  IndexIterator rhs) noexcept
            requires (!std::is_same_v<std::add_const_t<T>, T>) {
            PPR_ASSERT(lhs.getContainer() == rhs.m_container);
            return static_cast<difference_type>(lhs.getIndex())
                   - static_cast<difference_type>(rhs.m_index);
        }
    };

    template<typename ContainerT, std::integral IndexT = std::size_t>
        requires requires(ContainerT &arr, IndexT index)
        {
            { arr[index] };
        }
    IndexIterator(ContainerT &container, IndexT m_index) -> IndexIterator<ContainerT, typename ContainerT::value_type, IndexT>;

    // ------------------------------------------------------------------
    // Ranges for iterating over Bitmask bits (C++23)
    // ------------------------------------------------------------------

    // Iterates over indices of set bits only, in ascending order.
    // Uses countr_zero + reset — O(popcount) cost, zero wasted work.
    template<typename T = std::size_t, u32 N = bit_count_v<T> >
    struct SetBitsRange {
        using integral_type = unwrap_ref_decay_t<T>;

        struct sentinel {
        };

        struct iterator {
            using value_type = u32;
            using difference_type = std::ptrdiff_t;
            using iterator_category = std::input_iterator_tag;
            using iterator_concept = std::input_iterator_tag;

            integral_type m_remaining{0};

            [[nodiscard]] constexpr value_type operator*() const noexcept {
                return static_cast<u32>(std::countr_zero(m_remaining));
            }

            constexpr iterator &operator++() noexcept {
                // Clear the lowest set bit: isolate it with m & -m, then XOR it out.
                m_remaining &= m_remaining - integral_type(1);
                return *this;
            }

            constexpr iterator operator++(int) noexcept {
                auto copy = *this;
                ++(*this);
                return copy;
            }

            [[nodiscard]] constexpr bool operator==(sentinel) const noexcept {
                return m_remaining == integral_type(0);
            }
        };

        static_assert(std::input_iterator<iterator>);
        static_assert(std::sentinel_for<sentinel, iterator>);

        integral_type m_bits{};

        [[nodiscard]] constexpr iterator begin() const noexcept { return {m_bits}; }
        [[nodiscard]] static constexpr sentinel end() noexcept { return {}; }

        [[nodiscard]] constexpr bool empty() const noexcept { return m_bits == 0; }
        [[nodiscard]] constexpr u32 size() const noexcept { return static_cast<u32>(std::popcount(m_bits)); }
    };

    static_assert(std::ranges::input_range<SetBitsRange<> >);

    // ------------------------------------------------------------------
    // bit set using a single word
    // ------------------------------------------------------------------

    template<typename T = std::size_t, u32 N = bit_count_v<T> >
    struct Bitmask {
        using integral_type = unwrap_ref_decay_t<T>;

        static constexpr u32 bit_count_v = N;
        static constexpr u32 capacity_v = sizeof(T) * 8u;
        static constexpr u32 extra_bits_v = capacity_v - N;

        static constexpr integral_type all_v = ~integral_type{} >> extra_bits_v;

        T m_bits{zero_v};

        static constexpr Bitmask bitAnd(const std::initializer_list<T> bit_or) noexcept {
            Bitmask result;
            for (T mask: bit_or) {
                result &= mask;
            }
            return result;
        }

        static constexpr Bitmask bitOr(const std::initializer_list<T> bit_or) noexcept {
            Bitmask result;
            for (T mask: bit_or) {
                result |= mask;
            }
            return result;
        }

        [[nodiscard]] PPR_FORCE_INLINE static constexpr integral_type bitMask(const u32 bit) noexcept {
            PPR_ASSERT(bit < N);
            return static_cast<integral_type>(1) << bit;
        }

        [[nodiscard]] PPR_FORCE_INLINE constexpr integral_type &ref() noexcept {
            return m_bits;
        }

        [[nodiscard]] PPR_FORCE_INLINE constexpr integral_type cref() const noexcept {
            return static_cast<const integral_type &>(m_bits);
        }

        [[nodiscard]] constexpr bool test(const u32 bit) const noexcept {
            return (cref() & bitMask(bit)) != 0;
        }

        constexpr void set(const u32 bit) noexcept {
            ref() |= bitMask(bit);
        }

        constexpr void reset(const u32 bit) noexcept {
            ref() &= ~bitMask(bit);
        }

        constexpr void flip(const u32 bit) noexcept {
            ref() ^= bitMask(bit);
        }

        constexpr void setRange(const u32 offset, const u32 n) noexcept {
            PPR_ASSERT(offset + n <= bit_count_v);

            const integral_type mask = (bitMask(n) - 1u) << offset;
            PPR_ASSERT(not(mask & m_bits));

            m_bits |= mask;
        }

        constexpr void unsetRange(const u32 offset, const u32 n) noexcept {
            PPR_ASSERT(offset + n <= bit_count_v);

            const integral_type mask = (bitMask(n) - 1u) << offset;
            PPR_ASSERT(not(mask & ~m_bits));

            m_bits &= ~mask;
        }

        [[nodiscard]] constexpr bool all() const noexcept {
            return cref() == all_v;
        }

        [[nodiscard]] constexpr bool any() const noexcept {
            return cref() != 0u;
        }

        [[nodiscard]] constexpr bool none() const noexcept {
            return cref() == 0u;
        }

        constexpr void rotateLeft(const u32 shift) noexcept {
            PPR_ASSERT(shift < N);
            ref() = std::rotl(cref(), static_cast<int>(shift));
        }

        constexpr void rotateRight(const u32 shift) noexcept {
            PPR_ASSERT(shift < N);
            ref() = std::rotr(cref(), static_cast<int>(shift));
        }

        [[nodiscard]] constexpr u32 pop() noexcept {
            if (const u32 front = static_cast<u32>(std::countr_zero(m_bits)); front < N) [[likely]] {
                // Clear the lowest set bit: isolate it with m & -m, then XOR it out.
                m_bits &= m_bits - integral_type(1);
                return front;
            }
            return umax_v;
        }

        [[nodiscard]] constexpr u32 popAssumeNotEmpty() noexcept {
            PPR_ASSERT(any());
            const u32 front = static_cast<u32>(std::countr_zero(m_bits));
            // Clear the lowest set bit: isolate it with m & -m, then XOR it out.
            m_bits &= m_bits - integral_type(1);
            return front;
        }

        constexpr void setAll() noexcept {
            ref() = all_v;
        }

        constexpr void unsetAll() noexcept {
            ref() = 0u;
        }

        constexpr void fill(const bool enabled) noexcept {
            ref() = enabled ? all_v : 0u;
        }

        // Range over indices of set bits only — O(popcount), ascending order.
        //   for (u32 i : mask.eachBitSet()) { ... }
        [[nodiscard]] constexpr auto eachBitSet() const noexcept {
            return SetBitsRange<integral_type, N>{cref()};
        }

        [[nodiscard]] constexpr u32 countOnes() const noexcept {
            return static_cast<u32>(std::popcount(cref()));
        }

        [[nodiscard]] constexpr u32 countLeadingOnes() const noexcept {
            return static_cast<u32>(std::countl_one(cref() << extra_bits_v));
        }

        [[nodiscard]] constexpr u32 countTrailingOnes() const noexcept {
            return static_cast<u32>(std::countr_one(cref()));
        }

        [[nodiscard]] constexpr u32 countLeadingZeros() const noexcept {
            return static_cast<u32>(std::countl_zero(cref() << extra_bits_v));
        }

        [[nodiscard]] constexpr u32 countTrailingZeros() const noexcept {
            return static_cast<u32>(std::countr_zero(cref()));
        }

        [[nodiscard]] constexpr Bitmask<integral_type> byteSwap() const noexcept {
            return {std::byteswap(cref())};
        }

        [[nodiscard]] constexpr Bitmask<integral_type> invert() const noexcept {
            return {~cref()};
        }

        [[nodiscard]] static constexpr Bitmask<integral_type> setFirstN(const u32 n) noexcept {
            PPR_ASSERT(n <= N);
            return {all_v >> (N - n)};
        }

        [[nodiscard]] static constexpr Bitmask<integral_type> setLastN(const u32 n) noexcept {
            PPR_ASSERT(n <= N);
            return {~setFirstN(N - n).m_bits & all_v};
        }

        [[nodiscard]] static constexpr Bitmask<integral_type> unsetFirstN(const u32 n) noexcept {
            PPR_ASSERT(n <= N);
            return {all_v << n};
        }

        template<typename U> requires (Bitmask<U>::bit_count_v == bit_count_v)
        [[maybe_unused]] constexpr Bitmask &operator &=(const Bitmask<U> other) noexcept {
            ref() &= other.cref();
            return *this;
        }

        template<typename U> requires (Bitmask<U>::bit_count_v == bit_count_v)
        [[maybe_unused]] constexpr Bitmask &operator |=(const Bitmask<U> other) noexcept {
            ref() |= other.cref();
            return *this;
        }

        template<typename U> requires (Bitmask<U>::bit_count_v == bit_count_v)
        [[maybe_unused]] constexpr Bitmask &operator ^=(const Bitmask<U> other) noexcept {
            ref() ^= other.cref();
            return *this;
        }

        template<typename U> requires (Bitmask<U>::bit_count_v == bit_count_v)
        [[maybe_unused]] constexpr Bitmask &operator -=(const Bitmask<U> other) noexcept {
            ref() &= ~other.cref();
            return *this;
        }

        [[nodiscard]] constexpr Bitmask<integral_type> operator ~() const noexcept {
            return {~cref()};
        }

        template<typename U> requires (Bitmask<U>::bit_count_v == bit_count_v)
        [[nodiscard]] constexpr Bitmask<integral_type> operator &(const Bitmask<U> other) const noexcept {
            return {cref() & other.cref()};
        }

        template<typename U> requires (Bitmask<U>::bit_count_v == bit_count_v)
        [[nodiscard]] constexpr Bitmask<integral_type> operator |(const Bitmask<U> other) const noexcept {
            return {cref() | other.cref()};
        }

        template<typename U> requires (Bitmask<U>::bit_count_v == bit_count_v)
        [[nodiscard]] constexpr Bitmask<integral_type> operator ^(const Bitmask<U> other) const noexcept {
            return {cref() ^ other.cref()};
        }

        template<typename U> requires (Bitmask<U>::bit_count_v == bit_count_v)
        [[nodiscard]] constexpr Bitmask<integral_type> operator -(const Bitmask<U> other) const noexcept {
            return {cref() & ~other.cref()};
        }

        [[nodiscard]] constexpr Bitmask<integral_type> operator <<(const integral_type lshift) const noexcept {
            return {cref() << lshift};
        }

        [[nodiscard]] constexpr Bitmask<integral_type> operator >>(const integral_type rshift) const noexcept {
            return {cref() >> rshift};
        }

        template<typename U> requires (Bitmask<U>::bit_count_v == bit_count_v)
        [[nodiscard]] friend constexpr bool operator ==(const Bitmask lhs, const Bitmask<U> rhs) noexcept {
            return lhs.cref() == rhs.cref();
        }

        template<typename U> requires (Bitmask<U>::bit_count_v == bit_count_v)
        [[nodiscard]] friend constexpr std::strong_ordering operator<=>(const Bitmask lhs, const Bitmask<U> rhs) noexcept {
            return lhs.cref() <=> rhs.cref();
        }
    };

    template<typename T, std::size_t N = bit_count_v<T> >
    using BitmaskRef = Bitmask<std::reference_wrapper<T>, N>;

    template<typename T, std::size_t N>
    struct details::relocatable<Bitmask<T, N> > : std::true_type {
    };

    // ------------------------------------------------------------------
    // pack a pointer to a relative offset, can't be moved,
    // but it's copyable and serializable.
    // ------------------------------------------------------------------

    template<typename T, std::signed_integral OffsetT = std::ptrdiff_t>
    struct [[nodiscard]] RelPtr {
        using value_type = T;
        using pointer = std::add_pointer_t<T>;
        using reference = std::add_lvalue_reference_t<T>;

        OffsetT m_offset{0};

        constexpr RelPtr() noexcept = default;

        explicit constexpr RelPtr(pointer ptr PPR_LIFETIME_BOUND) noexcept {
            setData(ptr);
        }

        // ReSharper disable once CppNonExplicitConvertingConstructor
        constexpr RelPtr(std::nullptr_t) noexcept {
            setData(nullptr);
        }

        constexpr RelPtr(const RelPtr &other) noexcept
            : RelPtr(other.getData()) {
        }

        constexpr RelPtr &operator =(const RelPtr &other) & noexcept {
            setData(other.getData());
            return *this;
        }

        constexpr RelPtr(RelPtr &&) noexcept = delete;

        constexpr RelPtr &operator =(RelPtr &&) noexcept = delete;

        constexpr RelPtr &operator =(pointer ptr PPR_LIFETIME_BOUND) & noexcept {
            setData(ptr);
            return *this;
        }

        [[nodiscard]] PPR_FORCE_INLINE
        constexpr pointer getData() const & noexcept {
            if (m_offset == 0) {
                return nullptr;
            }

            // Use integer arithmetic to avoid pointer arithmetic UB
            return std::bit_cast<pointer>(
                std::bit_cast<std::uintptr_t>(this) +
                static_cast<std::uintptr_t>(m_offset));
        }

        PPR_FORCE_INLINE constexpr void setData(pointer ptr PPR_LIFETIME_BOUND) & noexcept {
            if (ptr == nullptr) {
                m_offset = 0;
                return;
            }

            // Use integer arithmetic to avoid pointer arithmetic UB
            m_offset = checked_cast<OffsetT>(
                std::bit_cast<std::intptr_t>(ptr) -
                std::bit_cast<std::intptr_t>(this));
        }

        // ------------------------------------------------------------------
        //  Null / validity
        // ------------------------------------------------------------------

        [[nodiscard]] PPR_FORCE_INLINE
        constexpr bool isNull() const noexcept {
            return m_offset == 0;
        }

        [[nodiscard]] PPR_FORCE_INLINE
        constexpr bool isValid() const noexcept {
            return m_offset != 0;
        }

        [[nodiscard]] PPR_FORCE_INLINE
        explicit constexpr operator bool() const noexcept {
            return isValid();
        }

        // ------------------------------------------------------------------
        //  Pointer-like operators
        // ------------------------------------------------------------------

        [[nodiscard]] PPR_FORCE_INLINE
        // ReSharper disable once CppNonExplicitConversionOperator
        constexpr operator pointer() const & noexcept {
            return getData();
        }

        [[nodiscard]] PPR_FORCE_INLINE
        constexpr pointer operator->() const & noexcept {
            return getData();
        }

        [[nodiscard]] PPR_FORCE_INLINE
        constexpr reference operator*() const & noexcept {
            return *getData();
        }

        [[nodiscard]] PPR_FORCE_INLINE
        constexpr reference operator[](const std::size_t offset) const & noexcept {
            return getData()[offset];
        }

        [[nodiscard]] PPR_FORCE_INLINE
        constexpr bool operator==(const RelPtr &other) const & noexcept {
            return getData() == other.getData();
        }

        [[nodiscard]] PPR_FORCE_INLINE
        constexpr std::strong_ordering operator<=>(const RelPtr &other) const & noexcept {
            return getData() <=> other.getData();
        }
    };

    template<typename T>
    RelPtr(T *ptr) -> RelPtr<T>;

    template<typename T, std::signed_integral OffsetT>
    struct details::relocatable<RelPtr<T, OffsetT> > : std::false_type {
        // must be moved to conserve absolute pointer address
    };

    // ------------------------------------------------------------------
    // allow packing a tag/flags into a pointer's unused bits
    // ------------------------------------------------------------------

    template<typename T, typename TagT = std::uintptr_t, std::align_val_t Alignment = alignof_v<T> >
    struct [[nodiscard]] TagPtr {
        static_assert(static_cast<std::uintptr_t>(Alignment) >= 1u,
                      "Alignment must be at least 1.");
        static_assert((static_cast<std::uintptr_t>(Alignment) & (static_cast<std::uintptr_t>(Alignment) - 1u)) == 0u,
                      "Alignment must be a power of two.");
        static_assert(sizeof(T *) == sizeof(std::uintptr_t),
                      "sizeof(T*) != sizeof(uintptr_t): pointer tagging is unsafe on this platform.");
        static_assert((std::bit_width(static_cast<std::uintptr_t>(Alignment)) - 1u) < sizeof(std::uintptr_t),
                      "Alignment consumes the entire pointer width; no bits remain for the address.");
        static_assert(sizeof(TagT) <= sizeof(std::uintptr_t),
                      "Tag type is too large to fit in the pointer's unused bits.");

        /// Number of flag bits available in the LSBs of the pointer.
        static constexpr std::size_t extra_bits = std::bit_width(static_cast<std::uintptr_t>(Alignment)) - 1u; // log2(Alignment)

        static constexpr std::uintptr_t FLAG_MASK = static_cast<std::uintptr_t>(Alignment) - 1u;
        static constexpr std::uintptr_t PTR_MASK = ~FLAG_MASK;

        std::uintptr_t m_packed{};

        constexpr TagPtr() noexcept = default;

        /// \pre (flags & PTR_MASK) == 0  — flags must fit inside extra_bits.
        /// \pre ptr is aligned to at least Alignment.
        explicit constexpr TagPtr(T *const ptr PPR_LIFETIME_BOUND, const TagT tag = default_value_v) noexcept {
            reset(ptr, tag);
        }

        constexpr void reset(T *const ptr, const TagT tag = default_value_v) noexcept {
            m_packed = std::bit_cast<std::uintptr_t>(ptr) | static_cast<std::uintptr_t>(tag);
            PPR_ASSERT((static_cast<std::uintptr_t>(tag) & PTR_MASK) == 0u
                && "TagPtr: flag value overflows the available LSBs.");
            PPR_ASSERT((std::bit_cast<std::uintptr_t>(ptr) & FLAG_MASK) == 0u
                && "TagPtr: pointer is not sufficiently aligned for the requested Alignment.");
        }

        /// Returns the pointer with all flag bits stripped.
        [[nodiscard]] PPR_FORCE_INLINE
        constexpr T *getData() const noexcept {
            return std::bit_cast<T *>(m_packed & PTR_MASK);
        }

        /// Returns the pointer with all flag bits stripped.
        template<typename U>
        [[nodiscard]] PPR_FORCE_INLINE
        constexpr U *getReinterpret() const noexcept {
            return std::bit_cast<U *>(m_packed & PTR_MASK);
        }

        /// Returns only the flag bits.
        [[nodiscard]] PPR_FORCE_INLINE
        constexpr TagT getTag() const noexcept {
            return static_cast<TagT>(m_packed & FLAG_MASK);
        }

        [[nodiscard]] PPR_FLATTEN
        constexpr std::tuple<T *, TagT> unpack() const noexcept {
            return std::make_tuple(getData(), getTag());
        }

        /// Returns only the flag bits.
        [[nodiscard]] PPR_FORCE_INLINE
        constexpr bool hasTag(const TagT tag) const noexcept {
            PPR_ASSERT((static_cast<std::uintptr_t>(tag) & PTR_MASK) == 0u);
            return (m_packed & static_cast<std::uintptr_t>(tag)) != 0u;
        }

        /// Replaces the pointer, preserving the current flags.
        PPR_FORCE_INLINE constexpr void setData(T *const ptr PPR_LIFETIME_BOUND) noexcept {
            PPR_ASSERT((std::bit_cast<std::uintptr_t>(ptr) & FLAG_MASK) == 0
                && "TagPtr: pointer is not sufficiently aligned for the requested Alignment.");
            m_packed = (m_packed & FLAG_MASK) | std::bit_cast<std::uintptr_t>(ptr);
        }

        /// Replaces ALL flags at once.
        PPR_FORCE_INLINE constexpr void setTag(const TagT tag) noexcept {
            PPR_ASSERT((static_cast<std::uintptr_t>(tag) & PTR_MASK) == 0
                && "TagPtr: tag value overflows the available LSBs.");
            m_packed = (m_packed & PTR_MASK) | static_cast<std::uintptr_t>(tag);
        }

        /// Returns a bitmask with only the extra bits
        [[nodiscard]] PPR_FORCE_INLINE
        constexpr auto getBits() const noexcept {
            return Bitmask<std::uintptr_t, extra_bits>(m_packed);
        }

        /// Returns a bitmask reference with only the extra bits
        [[nodiscard]] PPR_FORCE_INLINE
        constexpr auto getBits() noexcept {
            return BitmaskRef<std::uintptr_t, extra_bits>(m_packed);
        }

        // ------------------------------------------------------------------
        //  Null / validity
        // ------------------------------------------------------------------

        [[nodiscard]] PPR_FORCE_INLINE
        constexpr bool isNull() const noexcept {
            return getData() == nullptr;
        }

        [[nodiscard]] PPR_FORCE_INLINE
        constexpr bool isValid() const noexcept {
            return getData() != nullptr;
        }

        [[nodiscard]] PPR_FORCE_INLINE
        explicit constexpr operator bool() const noexcept {
            return isValid();
        }

        // ------------------------------------------------------------------
        //  Pointer-like operators
        // ------------------------------------------------------------------

        [[nodiscard]] PPR_FORCE_INLINE
        constexpr T *operator->() const noexcept {
            return getData();
        }

        [[nodiscard]] PPR_FORCE_INLINE
        constexpr T &operator*() const noexcept {
            return *getData();
        }

        [[nodiscard]] PPR_FORCE_INLINE
        constexpr T &operator[](const std::ptrdiff_t offset) const noexcept {
            return getData()[offset];
        }

        // ------------------------------------------------------------------
        //  Comparisons
        // ------------------------------------------------------------------

        /// Full equality: pointer AND tags must both match.
        [[nodiscard]] PPR_FORCE_INLINE
        constexpr bool operator==(const TagPtr other) const noexcept {
            return m_packed == other.m_packed;
        }

        /// Pointer-only equality: tags are ignored.
        [[nodiscard]] PPR_FORCE_INLINE
        constexpr bool operator==(const T *const ptr) const noexcept {
            return (m_packed & PTR_MASK) == std::bit_cast<std::uintptr_t>(ptr);
        }

        /// Pointer-only three-way comparison (tags ignored). Produces a
        /// consistent total order within a single execution.
        [[nodiscard]] PPR_FORCE_INLINE
        constexpr std::strong_ordering operator<=>(const TagPtr other) const noexcept {
            return (m_packed & PTR_MASK) <=> (other.m_packed & PTR_MASK);
        }

        [[nodiscard]] PPR_FORCE_INLINE
        constexpr std::strong_ordering operator<=>(const T *const ptr) const noexcept {
            return (m_packed & PTR_MASK) <=> std::bit_cast<std::uintptr_t>(ptr);
        }

        friend constexpr void swap(TagPtr &lhs, TagPtr &rhs) noexcept {
            std::swap(lhs.m_packed, rhs.m_packed);
        }
    };

    template<typename T, typename TagT, std::align_val_t Alignment>
    struct details::relocatable<TagPtr<T, TagT, Alignment> > : std::true_type {
    };

    // --------------------------------------------------------------
    // non-owning contiguous view helper
    // --------------------------------------------------------------

    template<typename T>
    struct [[nodiscard]] ArrayView {
        using value_type = const T;
        using pointer = std::add_pointer_t<value_type>;
        using reference = std::add_lvalue_reference_t<value_type>;
        using size_type = std::size_t;

        pointer m_data{};
        std::size_t m_size{};

        constexpr ArrayView() noexcept = default;

        constexpr ArrayView(pointer data PPR_LIFETIME_BOUND, const std::size_t size) noexcept
            : m_data(data),
              m_size(size) {
        }

        constexpr ArrayView(std::initializer_list<T> init_list PPR_LIFETIME_BOUND) noexcept
            : m_data(init_list.data()),
              m_size(init_list.size()) {
        }

        template<std::ranges::contiguous_range RangeT>
            requires std::convertible_to<std::add_pointer_t<std::ranges::range_reference_t<RangeT> >, pointer>
        // ReSharper disable once CppNonExplicitConvertingConstructor
        constexpr ArrayView(RangeT &&contiguous_range PPR_LIFETIME_BOUND) noexcept
            : m_data(std::ranges::data(contiguous_range)),
              m_size(std::ranges::size(contiguous_range)) {
        }

        [[nodiscard]] constexpr bool empty() const noexcept {
            return m_size == 0u;
        }

        [[nodiscard]] constexpr const T *data() const noexcept {
            return m_data;
        }

        [[nodiscard]] constexpr size_type size() const noexcept {
            return m_size;
        }

        [[nodiscard]] constexpr const T *begin() const noexcept {
            return m_data;
        }

        [[nodiscard]] constexpr const T *end() const noexcept {
            return m_data + m_size;
        }

        [[nodiscard]] constexpr const T &operator[](const size_type index) const noexcept {
            PPR_ASSERT(m_data && index < m_size);
            PPR_ASSUME(m_data != nullptr);
            return m_data[index];
        }
    };

    static_assert(std::ranges::contiguous_range<ArrayView<int> >);

    template<typename T>
    ArrayView(const T *data, std::size_t size) -> ArrayView<T>;

    template<typename T>
    ArrayView(std::initializer_list<T> list) -> ArrayView<T>;

    template<std::ranges::contiguous_range RangeT>
    ArrayView(RangeT &&contiguous_range) -> ArrayView<std::ranges::range_value_t<RangeT> >;

    template<typename T>
    struct details::relocatable<ArrayView<T> > : std::true_type {
    };

    // --------------------------------------------------------------
    // non-owning contiguous view helper with relative pointers (half the size of ArrayView<>)
    // --------------------------------------------------------------

    template<typename T>
    struct [[nodiscard]] RelativeView {
        using value_type = const T;
        using pointer = std::add_pointer_t<value_type>;
        using reference = std::add_lvalue_reference_t<value_type>;
        using size_type = u32;

        RelPtr<value_type, i32> m_data{};
        size_type m_size{};

        constexpr RelativeView() noexcept = default;

        constexpr RelativeView(pointer ptr PPR_LIFETIME_BOUND, const std::size_t n) noexcept {
            reset(ptr, n);
        }

        template<typename ValueT, std::size_t ExtentV = std::dynamic_extent>
            requires std::convertible_to<std::add_pointer_t<ValueT>, pointer>
        explicit constexpr RelativeView(const std::span<ValueT, ExtentV> &span) noexcept {
            reset(span.data(), span.size());
        }

        template<typename ValueT, std::size_t ExtentV = std::dynamic_extent>
            requires std::convertible_to<std::add_pointer_t<ValueT>, pointer>
        constexpr RelativeView &operator =(const std::span<ValueT, ExtentV> &span) noexcept {
            reset(span.data(), span.size());
            return *this;
        }

        // template<std::ranges::contiguous_range RangeT>
        //     requires std::convertible_to<std::add_pointer_t<std::ranges::range_reference_t<RangeT>>, pointer>
        // // ReSharper disable once CppNonExplicitConvertingConstructor
        // constexpr RelativeView(RangeT &&contiguous_range PPR_LIFETIME_BOUND) noexcept {
        //     reset(std::ranges::data(contiguous_range), std::ranges::size(contiguous_range));
        // }

        constexpr void reset(pointer ptr PPR_LIFETIME_BOUND, const std::size_t n) noexcept {
            m_data = ptr;
            m_size = safe_narrowing(n);
        }

        [[nodiscard]] constexpr bool empty() const noexcept {
            return m_size == 0u;
        }

        [[nodiscard]] constexpr pointer data() const noexcept {
            return m_data;
        }

        [[nodiscard]] constexpr size_type size() const noexcept {
            return m_size;
        }

        [[nodiscard]] constexpr pointer begin() const & noexcept {
            return m_data;
        }

        [[nodiscard]] constexpr pointer end() const & noexcept {
            return m_data + m_size;
        }

        [[nodiscard]] constexpr reference operator[](const std::size_t index) const & noexcept {
            PPR_ASSERT(m_data.isValid() && index < m_size);
            return m_data[index];
        }

        [[nodiscard]] constexpr std::span<value_type> span() const noexcept {
            return std::span<value_type>{m_data, m_size};
        }

        [[nodiscard]] constexpr ArrayView<T> view() const noexcept {
            return ArrayView<T>{m_data, m_size};
        }
    };

    static_assert(sizeof(RelativeView<int>) == sizeof(u64));
    static_assert(std::ranges::contiguous_range<RelativeView<int> >);

    template<typename T>
    RelativeView(ArrayView<T> view) -> RelativeView<T>;

    template<typename T>
    struct details::relocatable<RelativeView<T> > : std::false_type {
        // must be moved to conserve absolute pointer address
    };

    // ------------------------------------------------------------------
    // view transform adapter
    // ------------------------------------------------------------------

    template<typename T>
    class [[nodiscard]] TransformView {
        std23::function_ref<T(std::size_t) noexcept> m_transform;
        std::size_t m_size;

        template<std::ranges::random_access_range RangeT>
            requires std::convertible_to<std::ranges::range_value_t<RangeT>, T>
        static T transform_(const RangeT *const p_range, const std::size_t index) noexcept {
            return (*p_range)[index];
        }

    public:
        TransformView(const std::size_t size, std23::function_ref<T(std::size_t) noexcept> transform) noexcept
            : m_transform(std::move(transform)),
              m_size(size) {
        }

        template<std::ranges::random_access_range RangeT>
            requires std::convertible_to<std::ranges::range_value_t<RangeT>, T>
        explicit TransformView(const RangeT &range) noexcept
            : m_transform(std23::nontype<&transform_<RangeT>>, &range),
              m_size(std::ranges::size(range)) {
        }

        [[nodiscard]] bool empty() const noexcept {
            return m_size == 0u;
        }

        [[nodiscard]] std::size_t size() const noexcept {
            return m_size;
        }

        [[nodiscard]] T operator[](const std::size_t index) const noexcept {
            PPR_ASSERT(index < m_size);
            return m_transform(index);
        }

        using iterator = IndexIterator<const TransformView, T>;

        [[nodiscard]] iterator begin() const noexcept {
            return iterator(*this, 0u);
        }

        [[nodiscard]] iterator end() const noexcept {
            return iterator(*this, m_size);
        }
    };

    // ------------------------------------------------------------------
    // bounded single-thread stack
    // ------------------------------------------------------------------

    template<typename T, std::size_t N>
        requires std::is_trivial_v<T>
    struct Stack {
        using value_type = T;

        std::array<T, N> m_storage;
        std::size_t m_count{0};

        constexpr Stack() noexcept {
            PPR_EXPR_IF_DEBUG(mem::details::poisonFlood(mem::details::PoisonPattern::reserved,
                static_cast<void *>(m_storage.data()), N * sizeof(T)));
            mem::annotateEmptyContiguousContainer(m_storage.data(), N);
        }

        constexpr ~Stack() noexcept {
            mem::annotateContiguousContainer(m_storage.data(), N, m_count, 0u);
        }

        Stack &operator=(const Stack &other) noexcept {
            static_assert(std::is_trivially_destructible_v<T>);
            if (this != &other) [[likely]] {
                mem::annotateContiguousContainer(m_storage.data(), N, 0u, N);
                mem::annotateContiguousContainer(other.m_storage.data(), N, 0u, N);

                m_count = other.m_count;
                std::memcpy(m_storage.data(), other.m_storage.data(), N * sizeof(T));

                mem::annotateContiguousContainer(m_storage.data(), N, N, m_count);
                mem::annotateContiguousContainer(other.m_storage.data(), N, N, other.m_count);
            }
            return *this;
        }

        [[nodiscard]] constexpr bool isEmpty() const noexcept {
            return m_count == 0u;
        }

        [[nodiscard]] constexpr bool isFull() const noexcept {
            return m_count == N;
        }

        [[nodiscard]] constexpr std::size_t size() const noexcept {
            return m_count;
        }

        [[nodiscard]] constexpr T &operator[](const std::size_t index) noexcept {
            PPR_ASSERT(index < m_count);
            return m_storage[index];
        }

        [[nodiscard]] constexpr const T &operator[](const std::size_t index) const noexcept {
            PPR_ASSERT(index < m_count);
            return m_storage[index];
        }

        // Takes T by value — move or copy, caller's choice
        [[nodiscard]] constexpr bool push(T value) noexcept {
            if (m_count < N) [[likely]] {
                mem::annotateContiguousContainer(
                    m_storage.data(),
                    N,
                    m_count,
                    m_count + 1u);

                m_storage[m_count++] = std::move(value);
                return true;
            }
            return false;
        }

        constexpr void pushAssumeCapacity(T value) noexcept {
            PPR_ASSERT(m_count < N);
            mem::annotateContiguousContainer(
                m_storage.data(),
                N,
                m_count,
                m_count + 1u);

            m_storage[m_count++] = std::move(value);
        }

        [[nodiscard]] constexpr std::optional<T> pop() noexcept {
            if (m_count > 0) [[likely]] {
                const auto old_count = m_count;
                --m_count;
                T return_value = std::move(m_storage[m_count]);

                mem::annotateContiguousContainer(
                    m_storage.data(),
                    N,
                    old_count,
                    m_count);

                return return_value;
            }
            return std::nullopt;
        }

        [[nodiscard]] constexpr T popAssumeNotEmpty() noexcept {
            PPR_ASSERT(m_count > 0);
            const auto old_count = m_count;
            --m_count;
            T return_value = std::move(m_storage[m_count]);

            mem::annotateContiguousContainer(
                m_storage.data(),
                N,
                old_count,
                m_count);
            return return_value;
        }

        constexpr void clear() noexcept {
            mem::annotateContiguousContainer(m_storage.data(), N, m_count, 0u);
            m_count = 0u;
        }

        [[nodiscard]] constexpr T *begin() noexcept { return m_storage.data(); }
        [[nodiscard]] constexpr T *end() noexcept { return m_storage.data() + m_count; }

        [[nodiscard]] constexpr const T *begin() const noexcept { return m_storage.data(); }
        [[nodiscard]] constexpr const T *end() const noexcept { return m_storage.data() + m_count; }
    };

    template<typename T, std::size_t N> requires std::is_trivial_v<T>
    struct details::relocatable<Stack<T, N> > : relocatable<T> {
    };

    // ------------------------------------------------------------------
    // bounded single-thread ring-buffer
    // ------------------------------------------------------------------

    template<typename T, std::size_t N>
        requires std::is_trivial_v<T>
    struct RingBuffer {
        using value_type = T;

        [[nodiscard]] PPR_FORCE_INLINE static u32 arrIndex(const i32 pos) noexcept {
            if constexpr (std::has_single_bit(N)) {
                return static_cast<u32>(pos) & (static_cast<u32>(N) - 1u);
            } else {
                const i32 idx = pos % static_cast<i32>(N);
                return static_cast<u32>(idx < 0 ? idx + static_cast<i32>(N) : idx);
            }
        }

        std::array<T, N> m_storage;
        i32 m_back_pos{0};
        i32 m_front_pos{0};

#if PPR_ENABLE_SANITIZER_ADDRESS
        constexpr
        RingBuffer() noexcept {
            mem::poisonReserved(m_storage.data(), m_storage.size());
        }

        constexpr ~RingBuffer() noexcept {
            mem::poisonDestroyed(m_storage.data(), m_storage.size());
        }
#else
        constexpr RingBuffer() noexcept = default;
#endif

        [[nodiscard]] constexpr bool isEmpty() const noexcept {
            return m_back_pos == m_front_pos;
        }

        [[nodiscard]] constexpr bool isFull() const noexcept {
            const i32 count = m_back_pos - m_front_pos;
            return count == static_cast<i32>(N);
        }

        [[nodiscard]] constexpr std::size_t size() const noexcept {
            return checked_cast<std::size_t>(m_back_pos - m_front_pos);
        }

        [[nodiscard]] constexpr T &operator[](const std::size_t index) noexcept {
            PPR_ASSERT(static_cast<std::size_t>(m_front_pos) + index < static_cast<std::size_t>(m_back_pos));
            return m_storage[arrIndex(static_cast<i32>(static_cast<std::size_t>(m_front_pos) + index))];
        }

        [[nodiscard]] constexpr const T &operator[](const std::size_t index) const noexcept {
            PPR_ASSERT(static_cast<std::size_t>(m_front_pos) + index < static_cast<std::size_t>(m_back_pos));
            return m_storage[arrIndex(static_cast<i32>(static_cast<std::size_t>(m_front_pos) + index))];
        }

        template<typename... ArgsT> requires std::is_constructible_v<T, ArgsT &&...>
        [[nodiscard]] bool pushFront(ArgsT &&... args) noexcept(std::is_nothrow_constructible_v<T, ArgsT &&...>) {
            if (!isFull()) [[likely]] {
                pushFrontAssumeNotFull(std::forward<ArgsT>(args)...);
                return true;
            }
            return false;
        }

        template<typename... ArgsT> requires std::is_constructible_v<T, ArgsT &&...>
        void pushFrontAssumeNotFull(ArgsT &&... args) noexcept(std::is_nothrow_constructible_v<T, ArgsT &&...>) {
            PPR_ASSERT(!isFull());
            --m_front_pos;
            T &dst = m_storage[arrIndex(m_front_pos)];
            mem::unpoisonUninitialized(&dst);
            std::construct_at(&dst, std::forward<ArgsT>(args)...);
        }

        template<typename... ArgsT> requires std::is_constructible_v<T, ArgsT &&...>
        [[nodiscard]] bool pushBack(ArgsT &&... args) noexcept(std::is_nothrow_constructible_v<T, ArgsT &&...>) {
            if (!isFull()) [[likely]] {
                pushBackAssumeNotFull(std::forward<ArgsT>(args)...);
                return true;
            }
            return false;
        }

        template<typename... ArgsT> requires std::is_constructible_v<T, ArgsT &&...>
        void pushBackAssumeNotFull(ArgsT &&... args) noexcept(std::is_nothrow_constructible_v<T, ArgsT &&...>) {
            PPR_ASSERT(!isFull());
            T &dst = m_storage[arrIndex(m_back_pos)];
            mem::unpoisonUninitialized(&dst);
            std::construct_at(&dst, std::forward<ArgsT>(args)...);
            ++m_back_pos;
        }

        [[nodiscard]] std::optional<T> popBack() noexcept {
            if (!isEmpty()) [[likely]] {
                return popBackAssumeNotEmpty();
            }
            m_front_pos = m_back_pos = 0;
            return std::nullopt;
        }

        [[nodiscard]] T popBackAssumeNotEmpty() noexcept {
            PPR_ASSERT(!isEmpty());
            --m_back_pos;
            T &src = m_storage[arrIndex(m_back_pos)];
            T return_value = std::move(src);
            mem::poisonDestroyed(&src);
            return return_value; // NRVO applies
        }

        [[nodiscard]] std::optional<T> popFront() noexcept {
            if (!isEmpty()) [[likely]] {
                T &src = m_storage[arrIndex(m_front_pos)];
                T return_value = std::move(src);
                mem::poisonDestroyed(&src);
                ++m_front_pos;
                return return_value; // NRVO applies
            }
            m_front_pos = m_back_pos = 0;
            return std::nullopt;
        }

        [[nodiscard]] T popFrontAssumeNotEmpty() noexcept {
            PPR_ASSERT(!isEmpty());
            T &src = m_storage[arrIndex(m_front_pos)];
            T value = std::move(src);
            mem::poisonDestroyed(&src);
            ++m_front_pos;
            return value; // NRVO applies
        }

        void clear() noexcept {
            static_assert(std::is_trivially_destructible_v<T>);
            mem::poisonReserved(m_storage.data(), m_storage.size());
            m_back_pos = m_front_pos = 0;
        }

        using iterator = IndexIterator<RingBuffer, T, i32>;
        using const_iterator = IndexIterator<const RingBuffer, const T, i32>;

        [[nodiscard]] constexpr iterator begin() noexcept { return iterator(*this, m_front_pos); }
        [[nodiscard]] constexpr iterator end() noexcept { return iterator(*this, m_back_pos); }

        [[nodiscard]] constexpr const_iterator begin() const noexcept { return const_iterator(*this, m_front_pos); }
        [[nodiscard]] constexpr const_iterator end() const noexcept { return const_iterator(*this, m_back_pos); }
    };

    template<typename T, std::size_t N> requires std::is_trivial_v<T>
    struct details::relocatable<RingBuffer<T, N> > : relocatable<T> {
    };

    // ------------------------------------------------------------------
    // sorting
    // ------------------------------------------------------------------

    namespace sort {
        // ------------------------------------------------------------------
        // Generic in-place Shell sort — zero allocation, constexpr,
        // std::ranges-style interface with projection support.
        //
        // Gap sequence: Sedgewick (1982)
        //   h(0)   = 1
        //   h(k)   = 4^k + 3·2^(k−1) + 1   for k ≥ 1
        //            → 1, 8, 23, 77, 281, 1073, 4193, 16577, 65921 …
        //
        // Complexity:
        //   Worst case  O(n^(4/3))  — proven, Sedgewick 1982
        //   Average     O(n^(7/6))  — empirical
        //
        // NOTE: The 1986 Sedgewick sequence (1, 5, 19, 41, 109, 209, …),
        // generated by two interleaved sub-sequences, achieves the same
        // O(n^(4/3)) bound with better constants in practice.  This
        // implementation uses the simpler 1982 single-formula sequence.
        // Pratt's sequence (2^i·3^j) gives a proven O(n log² n) bound at
        // the cost of more passes for small n; prefer it if worst-case
        // guarantees matter more than average throughput.
        //
        // The gap table is fully generated at compile time by consteval;
        // no runtime computation beyond loading the pre-built array.
        //
        // noexcept contract: this function is noexcept iff comparison,
        // projection, iter_move, and move-assignment are all noexcept.
        // If any of those operations throw, std::terminate is called.
        // Callers using comparators or projections that may throw should
        // not rely on a noexcept sort; std::ranges::sort makes the same
        // trade-off for the same reason.
        //
        // Non-sized sentinels: when S does not model sized_sentinel_for<IteratorT>,
        // std::ranges::distance(first, last) is O(n).  For sized ranges
        // (std::span, std::vector, std::array, sized views) it is O(1).
        // ------------------------------------------------------------------

        namespace details {
            // ---------------------------------------------------------------
            // Generates the first N terms of the Sedgewick (1982) sequence.
            // 16 terms covers arrays up to ~1.07 billion elements (4^15 ≈ 1.07B).
            // consteval forces full evaluation at compile time — zero runtime cost.
            // ---------------------------------------------------------------
            consteval auto makeSedgewickGaps() noexcept {
                constexpr std::size_t N = 16u;
                std::array<std::size_t, N> gaps{};

                gaps[0] = 1u;

                // At iteration k (1-based):
                //   p4 == 4^k
                //   p2 == 2^(k-1)
                // Formula: gaps[k] = 4^k + 3·2^(k-1) + 1
                std::size_t p4 = 4u; // 4^1
                std::size_t p2 = 1u; // 2^0

                for (std::size_t k = 1u; k < N; ++k, p4 *= 4u, p2 *= 2u) {
                    gaps[k] = p4 + 3u * p2 + 1u;
                }

                return gaps;
            }

            inline constexpr auto sedgewick_gaps_v = makeSedgewickGaps();

            // ---------------------------------------------------------------
            // Concept: checks that comparison, projection, iter_move, and
            // move-assignment are all noexcept for iterator IteratorT.
            // Used to propagate the noexcept specifier correctly.
            // ---------------------------------------------------------------
            template<typename IteratorT, typename CompT, typename ProjT>
            concept TNothrowSortable =
                    noexcept(std::declval<CompT &>()(
                        std::invoke(std::declval<ProjT &>(), *std::declval<IteratorT>()),
                        std::invoke(std::declval<ProjT &>(), *std::declval<IteratorT>()))) &&
                    noexcept(std::ranges::iter_move(std::declval<IteratorT>())) &&
                    noexcept(*std::declval<IteratorT>() = std::ranges::iter_move(std::declval<IteratorT>()));
        } // namespace details

        // ------------------------------------------------------------------
        // Iterator-pair overload — accepts any random-access iterator with
        // a compatible sentinel, plus optional comparator and projection.
        // ------------------------------------------------------------------
        template<
            std::random_access_iterator IteratorT,
            std::sentinel_for<IteratorT> S,
            typename CompT = std::ranges::less,
            typename ProjT = std::identity>
            requires std::sortable<IteratorT, CompT, ProjT>
        constexpr void inplaceShell(
            IteratorT first,
            S last,
            CompT comp = {},
            ProjT proj = {}
        ) noexcept(details::TNothrowSortable<IteratorT, CompT, ProjT>) {
            const auto n = static_cast<std::size_t>(std::ranges::distance(first, last));
            if (n < 2u) [[unlikely]] return;

            auto gap_idx =
                    static_cast<std::ptrdiff_t>(details::sedgewick_gaps_v.size()) - 1;

            while (gap_idx > 0 && details::sedgewick_gaps_v[static_cast<std::size_t>(gap_idx)] >= n)
                --gap_idx;

            // Outer loop: shrinking gaps, coarsest → finest (gap == 1 last).
            for (; gap_idx >= 0; --gap_idx) {
                const auto gap = static_cast<std::iter_difference_t<IteratorT>>(
                    details::sedgewick_gaps_v[static_cast<std::size_t>(gap_idx)]);

                for (auto it = first + gap; it != last; ++it) {
                    auto key = std::ranges::iter_move(it);
                    auto j = it;

                    // Compute (j - gap) directly in the loop checks to avoid out-of-bounds decrements
                    while (j - first >= gap
                           && std::invoke(comp,
                                          std::invoke(proj, key),
                                          std::invoke(proj, *(j - gap)))) {
                        *j = std::ranges::iter_move(j - gap);
                        j -= gap;
                    }
                    *j = std::move(key);
                }
            }
        }

        // ------------------------------------------------------------------
        // Range overload — accepts any random-access range:
        // std::span, std::array, std::vector, views::take results, etc.
        // ------------------------------------------------------------------
        template<
            std::ranges::random_access_range R,
            typename CompT = std::ranges::less,
            typename ProjT = std::identity>
            requires std::sortable<std::ranges::iterator_t<R>, CompT, ProjT>
        constexpr void inplaceShell(
            R &&r,
            CompT comp = {},
            ProjT proj = {}
        ) noexcept(
            noexcept(inplaceShell(
                std::ranges::begin(std::forward<R>(r)),
                std::ranges::end(std::forward<R>(r)),
                std::move(comp),
                std::move(proj)))
        ) {
            inplaceShell(
                std::ranges::begin(std::forward<R>(r)),
                std::ranges::end(std::forward<R>(r)),
                std::move(comp),
                std::move(proj));
        }
    }

    template<hash::THashable T>
    struct details::relocatable<hash::Memoizer<T> > : relocatable<T> {
    };
}
