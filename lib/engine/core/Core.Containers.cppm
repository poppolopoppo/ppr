module;
#include "pP/Macros.h"
#include "rapidhash.h"

export module engine.core:containers;

import :assert;
import :hal;

import std;

export namespace pP {
    namespace details {
        template<std::forward_iterator IteratorT, typename T>
        inline constexpr bool is_iterator_of = std::is_convertible_v<std::iter_value_t<IteratorT>, T>;

        template<typename T, typename LhsT, typename RhsT = LhsT>
        concept TEqualTo = requires(const std::remove_cvref_t<T> &cmp, const std::remove_cvref_t<LhsT> &lhs, const std::remove_cvref_t<RhsT> &rhs)
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


    // ------------------------------------------------------------------
    // general purpose index iterator with random access
    // ------------------------------------------------------------------

    template<typename ContainerT, typename T, std::integral IndexT = std::size_t>
        requires requires(ContainerT &arr, IndexT index)
        {
            { arr[index] } -> std::convertible_to<std::add_lvalue_reference_t<T> >;
        }
    class IndexIterator {
    public:
        using iterator_category = std::random_access_iterator_tag;
        using iterator_concept = std::random_access_iterator_tag; // C++20+
        using difference_type = std::make_signed_t<IndexT>;
        using value_type = T;
        using pointer = std::add_pointer_t<value_type>;
        using reference = std::add_lvalue_reference_t<value_type>;

    private:
        ContainerT *m_container{nullptr};
        IndexT m_index{0};

    public:
        // ------------------------------------------------------------
        // constructors
        // ------------------------------------------------------------
        constexpr IndexIterator() noexcept = default;

        constexpr IndexIterator(ContainerT &container, const IndexT index) noexcept
            : m_container(std::addressof(container)), m_index(index) {
        }

        // allow conversion to const iterator
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

        [[nodiscard]] constexpr pointer operator->() const noexcept {
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
        [[nodiscard]] constexpr sentinel end() const noexcept { return {}; }

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
        static constexpr u32 capacity_v = sizeof(T)*8u;
        static constexpr u32 extra_bits_v = capacity_v - N;

        static constexpr integral_type all_v = ~integral_type{} >> extra_bits_v;

        T m_bits{zero_v};

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
        OffsetT m_offset{0};

        constexpr RelPtr() noexcept = default;

        explicit constexpr RelPtr(T *ptr) noexcept {
            setData(ptr);
        }

        constexpr RelPtr(const RelPtr &other) noexcept
            : RelPtr(other.getData()) {
        }

        constexpr RelPtr &operator =(const RelPtr &other) noexcept {
            setData(other.getData());
            return *this;
        }

        constexpr RelPtr(RelPtr &&) noexcept = delete;

        constexpr RelPtr &operator =(RelPtr &&) noexcept = delete;

        [[nodiscard]] PPR_FORCE_INLINE
        constexpr T *getData() const noexcept {
            return m_offset ? std::bit_cast<T *>(this) + static_cast<std::ptrdiff_t>(m_offset) * static_cast<std::ptrdiff_t>(alignof(T)) : nullptr;
        }

        /// Replaces the pointer, preserving the current flags.
        PPR_FORCE_INLINE constexpr void setData(T *ptr) noexcept {
            if (ptr == nullptr) {
                m_offset = 0;
                return;
            }

            const auto ptr_diff = static_cast<std::ptrdiff_t>(ptr - std::bit_cast<T *>(this));
            PPR_ASSERT(ptr_diff % static_cast<std::ptrdiff_t>(alignof(T)) == 0);
            m_offset = checked_cast<OffsetT>(static_cast<std::size_t>(ptr_diff) / alignof(T));
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
        constexpr operator T *() const noexcept {
            return getData();
        }

        [[nodiscard]] PPR_FORCE_INLINE
        constexpr T *operator->() const noexcept {
            return getData();
        }

        [[nodiscard]] PPR_FORCE_INLINE
        constexpr T &operator*() const noexcept {
            return *getData();
        }

        // ------------------------------------------------------------------
        //  Comparisons
        // ------------------------------------------------------------------

        /// Full equality: pointer AND tags must both match.
        [[nodiscard]] PPR_FORCE_INLINE
        constexpr bool operator==(const RelPtr &other) const noexcept {
            return getData() == other.getData();
        }

        /// Pointer-only three-way comparison (tags ignored). Produces a
        /// consistent total order within a single execution.
        [[nodiscard]] PPR_FORCE_INLINE
        constexpr std::strong_ordering operator<=>(const RelPtr &other) const noexcept {
            return getData() <=> other.getData();
        }
    };

    template<typename T>
    RelPtr(T *ptr) -> RelPtr<T>;

    template<typename T, std::signed_integral OffsetT>
    struct details::relocatable<RelPtr<T, OffsetT> > : std::true_type {
    };

    // ------------------------------------------------------------------
    // allow packing a tag/flags into a pointer's unused bits
    // ------------------------------------------------------------------

    template<typename T, typename TagT = std::uintptr_t, std::align_val_t Alignment = alignof_v<T> >
    struct [[nodiscard]] TagPtr {
        static_assert(static_cast<std::uintptr_t>(Alignment) >= 1,
                      "Alignment must be at least 1.");
        static_assert((static_cast<std::uintptr_t>(Alignment) & static_cast<std::uintptr_t>(Alignment) - 1) == 0,
                      "Alignment must be a power of two.");
        static_assert(sizeof(T *) == sizeof(std::uintptr_t),
                      "sizeof(T*) != sizeof(uintptr_t): pointer tagging is unsafe on this platform.");
        static_assert(std::bit_width(static_cast<std::uintptr_t>(Alignment)) - 1 < sizeof(std::uintptr_t),
                      "Alignment consumes the entire pointer width; no bits remain for the address.");
        static_assert(sizeof(TagT) <= sizeof(std::uintptr_t),
                      "Tag type is too large to fit in the pointer's unused bits.");

        /// Number of flag bits available in the LSBs of the pointer.
        static constexpr std::size_t extra_bits = std::bit_width(static_cast<std::uintptr_t>(Alignment)) - 1; // log2(Alignment)

        static constexpr std::uintptr_t FLAG_MASK = static_cast<std::uintptr_t>(Alignment) - 1u;
        static constexpr std::uintptr_t PTR_MASK = ~FLAG_MASK;

        std::uintptr_t m_packed{};

        constexpr TagPtr() noexcept = default;

        /// \pre (flags & PTR_MASK) == 0  — flags must fit inside extra_bits.
        /// \pre ptr is aligned to at least Alignment.
        explicit constexpr TagPtr(T *const ptr, const TagT tag = default_value_v) noexcept {
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

        /// Returns only the flag bits.
        [[nodiscard]] PPR_FORCE_INLINE
        constexpr bool hasTag(const TagT tag) const noexcept {
            PPR_ASSERT((static_cast<std::uintptr_t>(tag) & PTR_MASK) == 0u);
            return (m_packed & static_cast<std::uintptr_t>(tag)) != 0u;
        }

        /// Replaces the pointer, preserving the current flags.
        PPR_FORCE_INLINE constexpr void setData(T *const ptr) noexcept {
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

    // ------------------------------------------------------------------
    // bounded single-thread stack
    // ------------------------------------------------------------------

    template<typename T, std::size_t N> requires std::is_trivial_v<T>
    struct Stack {
        using value_type = T;

        std::array<T, N> m_storage;
        std::size_t m_count{0};

        constexpr Stack() noexcept = default;

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
                m_storage[m_count++] = value;
                return true;
            }
            return false;
        }

        constexpr void pushAssumeCapacity(T value) noexcept {
            PPR_ASSERT(m_count < N);
            m_storage[m_count++] = value;
        }

        [[nodiscard]] constexpr std::optional<T> pop() noexcept {
            if (m_count > 0) [[likely]] {
                return m_storage[--m_count];
            }
            return std::nullopt;
        }

        [[nodiscard]] constexpr T popAssumeNotEmpty() noexcept {
            PPR_ASSERT(m_count > 0);
            return m_storage[--m_count];
        }

        void clear() noexcept {
            m_count = 0u;
        }

        using iterator = IndexIterator<Stack, T>;
        using const_iterator = IndexIterator<const Stack, const T>;

        [[nodiscard]] constexpr iterator begin() noexcept { return iterator(*this, 0u); }
        [[nodiscard]] constexpr iterator end() noexcept { return iterator(*this, m_count); }

        [[nodiscard]] constexpr const_iterator begin() const noexcept { return const_iterator(*this, 0u); }
        [[nodiscard]] constexpr const_iterator end() const noexcept { return const_iterator(*this, m_count); }

        [[nodiscard]] constexpr auto each(this auto &&self) noexcept {
            return self.m_storage | std::views::take(self.m_count);
        }
    };

    template<typename T, std::size_t N> requires std::is_trivial_v<T>
    struct details::relocatable<Stack<T, N> > : relocatable<T> {
    };

    // ------------------------------------------------------------------
    // bounded single-thread ring-buffer
    // ------------------------------------------------------------------

    template<typename T, std::size_t N> requires std::is_trivial_v<T>
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

        constexpr RingBuffer() noexcept = default;

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
            m_storage[arrIndex(m_front_pos)] = T{std::forward<ArgsT>(args)...};
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
            m_storage[arrIndex(m_back_pos)] = T{std::forward<ArgsT>(args)...};
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
            return m_storage[arrIndex(m_back_pos)]; // NRVO applies
        }

        [[nodiscard]] std::optional<T> popFront() noexcept {
            if (!isEmpty()) [[likely]] {
                T value = m_storage[arrIndex(m_front_pos)];
                ++m_front_pos;
                return value; // NRVO applies
            }
            m_front_pos = m_back_pos = 0;
            return std::nullopt;
        }

        [[nodiscard]] T popFrontAssumeNotEmpty() noexcept {
            PPR_ASSERT(!isEmpty());
            T value = m_storage[arrIndex(m_front_pos)];
            ++m_front_pos;
            return value; // NRVO applies
        }

        void clear() noexcept {
            static_assert(std::is_trivially_destructible_v<T>);
            m_back_pos = m_front_pos = 0;
        }

        using iterator = IndexIterator<RingBuffer, T, i32>;
        using const_iterator = IndexIterator<const RingBuffer, const T, i32>;

        [[nodiscard]] constexpr iterator begin() noexcept { return iterator(*this, m_front_pos); }
        [[nodiscard]] constexpr iterator end() noexcept { return iterator(*this, m_back_pos); }

        [[nodiscard]] constexpr const_iterator begin() const noexcept { return const_iterator(*this, m_front_pos); }
        [[nodiscard]] constexpr const_iterator end() const noexcept { return const_iterator(*this, m_back_pos); }

        [[nodiscard]] auto each(this auto &&self) noexcept {
            return std::ranges::subrange(self.begin(), self.end());
        }
    };

    template<typename T, std::size_t N> requires std::is_trivial_v<T>
    struct details::relocatable<RingBuffer<T, N> > : relocatable<T> {
    };

    // ------------------------------------------------------------------
    // recycler from go
    // ------------------------------------------------------------------

    template<typename T, std::size_t N>
    class Recycler {
        Stack<T, N> m_free_blocks{};

    public:
        Recycler() noexcept = default;

        constexpr ~Recycler() noexcept {
            PPR_ASSERT(m_free_blocks.isEmpty() && "There are still blocks in-flight while destroying the recycler");
        }

        [[nodiscard]] constexpr bool allocate(T &out_recycled) noexcept {
            if (auto recycled = m_free_blocks.pop()) [[likely]] {
                out_recycled = std::move(*recycled);
                return true;
            }
            return false;
        }

        [[nodiscard]] bool release(T value) noexcept {
            return m_free_blocks.push(value);
        }

        template<typename DestructorT> requires std::is_invocable_v<DestructorT, T &&>
        void shrinkToFit(DestructorT destroy) noexcept {
            for (T &block : m_free_blocks) {
                destroy(block);
            }
            m_free_blocks.clear();
        }
    };

    // ------------------------------------------------------------------
    // hashing
    // ------------------------------------------------------------------

    struct hash_t {
        std::size_t m_value{0u};

        [[nodiscard]] friend constexpr bool operator==(const hash_t lhs, const hash_t rhs) noexcept {
            return lhs.m_value == rhs.m_value;
        }

        [[nodiscard]] friend constexpr std::strong_ordering operator<=>(const hash_t lhs, const hash_t rhs) noexcept {
            return lhs.m_value <=> rhs.m_value;
        }

        friend constexpr void swap(hash_t &lhs, hash_t &rhs) noexcept {
            std::swap(lhs.m_value, rhs.m_value);
        }

        [[nodiscard]] friend constexpr hash_t hashValue(const hash_t value) noexcept {
            return value; // pass-through
        }
    };

    namespace hash {
        // hash_mix for 64 bit size_t
        //
        // The general "xmxmx" form of state of the art 64 bit mixers originates
        // from Murmur3 by Austin Appleby, which uses the following function as
        // its "final mix":
        //
        //	k ^= k >> 33;
        //	k *= 0xff51afd7ed558ccd;
        //	k ^= k >> 33;
        //	k *= 0xc4ceb9fe1a85ec53;
        //	k ^= k >> 33;
        //
        // (https://github.com/aappleby/smhasher/blob/master/src/MurmurHash3.cpp)
        //
        // It has subsequently been improved multiple times by different authors
        // by changing the constants. The most well known improvement is the
        // so-called "variant 13" function by David Stafford:
        //
        //	k ^= k >> 30;
        //	k *= 0xbf58476d1ce4e5b9;
        //	k ^= k >> 27;
        //	k *= 0x94d049bb133111eb;
        //	k ^= k >> 31;
        //
        // (https://zimbry.blogspot.com/2011/09/better-bit-mixing-improving-on.html)
        //
        // This mixing function is used in the splitmix64 RNG:
        // http://xorshift.di.unimi.it/splitmix64.c
        //
        // We use Jon Maiga's implementation from
        // http://jonkagstrom.com/mx3/mx3_rev2.html
        //
        // 	x ^= x >> 32;
        //	x *= 0xe9846af9b1a615d;
        //	x ^= x >> 32;
        //	x *= 0xe9846af9b1a615d;
        //	x ^= x >> 28;
        //
        // An equally good alternative is Pelle Evensen's Moremur:
        //
        //	x ^= x >> 27;
        //	x *= 0x3C79AC492BA7B653;
        //	x ^= x >> 33;
        //	x *= 0x1C69B3F74AC4AE35;
        //	x ^= x >> 27;
        //
        // (https://mostlymangling.blogspot.com/2019/12/stronger-better-morer-moremur-better.html)

        [[nodiscard]] constexpr u64 mix(u64 x) noexcept {
            std::uint64_t const m = 0xe9846af9b1a615d;

            x ^= x >> 32;
            x *= m;
            x ^= x >> 32;
            x *= m;
            x ^= x >> 28;

            return x;
        }

        // hash_mix for 32 bit size_t
        //
        // We use the "best xmxmx" implementation from
        // https://github.com/skeeto/hash-prospector/issues/19

        [[nodiscard]] constexpr u32 mix(u32 x) noexcept {
            std::uint32_t const m1 = 0x21f0aaad;
            std::uint32_t const m2 = 0x735a2d97;

            x ^= x >> 16;
            x *= m1;
            x ^= x >> 15;
            x *= m2;
            x ^= x >> 15;

            return x;
        }

        // https://github.com/boostorg/container_hash/blob/e3cbbebc8a1f9833287c8eb52fb0484ba744646b/include/boost/container_hash/hash.hpp#L470
        [[nodiscard]] constexpr u32 combine(u32 s, u32 h) noexcept {
            return mix(s + 0x9e3779b9 + h);
        }

        // https://github.com/boostorg/container_hash/blob/e3cbbebc8a1f9833287c8eb52fb0484ba744646b/include/boost/container_hash/hash.hpp#L470
        [[nodiscard]] constexpr u64 combine(u64 s, u64 h) noexcept {
            return mix(s + 0x9e3779b9 + h);
        }

        [[nodiscard]] constexpr hash_t combine(const hash_t seed, const hash_t hash_value) noexcept {
            return hash_t{combine(seed.m_value, hash_value.m_value)};
        }

        // duplicated from rapidhash to solve linker issues due to modules
        inline constexpr u64 rapid_secret_v[8] = {
            0x2d358dccaa6c78a5ull,
            0x8bb84b93962eacc9ull,
            0x4b33a62ed433d4a3ull,
            0x4d5a2da51de1aa47ull,
            0xa0761d6478bd642full,
            0xe7037ed1a0b428dbull,
            0x90ed1765281c388cull,
            0xaaaaaaaaaaaaaaaaull
        };

        // Used by Chromium, Folly's F14, Fuchsia, Ninja, JuliaLang, ziglang, fb303, zxc, among others
        [[nodiscard]] RAPIDHASH_ALWAYS_INLINE hash_t memory(const void *const key, const std::size_t size_bytes, const u64 seed) noexcept {
            return hash_t{rapidhash_internal(key, size_bytes, seed, rapid_secret_v)};
        }

        // Used by Chromium, Folly's F14, Fuchsia, Ninja, JuliaLang, ziglang, fb303, zxc, among others
        [[nodiscard]] RAPIDHASH_ALWAYS_INLINE hash_t small(const void *const key, const std::size_t size_bytes, const u64 seed) noexcept {
            return hash_t{rapidhashMicro_internal(key, size_bytes, seed, rapid_secret_v)};
        }

        // Used by Chromium, Folly's F14, Fuchsia, Ninja, JuliaLang, ziglang, fb303, zxc, among others
        template<typename T> requires details::is_relocatable_v<T>
        [[nodiscard]] RAPIDHASH_ALWAYS_INLINE hash_t trivial(const T *const trivial, const u64 seed) noexcept {
            return hash_t{::rapidhashNano_internal(trivial, sizeof(T), seed, rapid_secret_v)};
        }

        template<typename T, typename ValueT>
        concept THasher = requires(const std::remove_cvref_t<T> &hasher, const ValueT &value)
        {
            { hasher(value) } -> std::same_as<hash_t>;
        };

        inline constexpr u64 default_seed_v = 0xA64E'B204'80BD'0F29ull;

        template<typename T>
        [[nodiscard]] PPR_FLATTEN RAPIDHASH_ALWAYS_INLINE hash_t ptr(const T *const p) noexcept {
            const std::uintptr_t x = std::bit_cast<std::uintptr_t>(p);
            // `x + (x >> 3)` adjustment by Alberto Barbati and Dave Harris.
            return hash_t{mix(x + (x >> 3))};
        }
    }

    template<typename EnumT> requires std::is_enum_v<EnumT>
    [[nodiscard]] PPR_FLATTEN constexpr hash_t hashValue(const EnumT value) noexcept {
        return hash::trivial(&value, hash::default_seed_v);
    }

    template<std::integral IntegralT>
    [[nodiscard]] PPR_FLATTEN constexpr hash_t hashValue(const IntegralT value) noexcept {
        return hash::trivial(&value, hash::default_seed_v);
    }

    template<std::floating_point FloatingPointT>
    [[nodiscard]] PPR_FLATTEN constexpr hash_t hashValue(const FloatingPointT value) noexcept {
        return hash::trivial(&value, hash::default_seed_v);
    }

    namespace hash {
        template<typename T>
        concept THashable = requires(const std::remove_cvref_t<T> &value)
        {
            { hashValue(value) } -> std::same_as<hash_t>;
        };

        template<THashable T>
        struct DefaultHash {
            constexpr hash_t operator ()(const T &value) const noexcept {
                return hashValue(value);
            }
        };

        template<THashable HashableValueT>
        [[nodiscard]] constexpr hash_t combine(const hash_t seed, const HashableValueT &value) noexcept {
            return combine(seed, hashValue(value));
        }

        template<std::ranges::sized_range SizedRangeT>
        [[nodiscard]] PPR_FLATTEN constexpr hash_t sizedRange(SizedRangeT &&values) noexcept
            requires THashable<std::ranges::range_value_t<SizedRangeT> > {
            hash_t H = hashValue(std::ranges::size(values));
            for (const auto &value: values) {
                H = hash::combine(H, value);
            }
            return H;
        }

        template<std::ranges::range UnorderedRangeT>
        [[nodiscard]] PPR_FLATTEN constexpr hash_t unorderedRange(UnorderedRangeT &&values) noexcept
            requires THashable<std::ranges::range_value_t<UnorderedRangeT> > {
            hash_t H{default_seed_v};
            for (const auto &value: values) {
                // use an associative hash combine, so the final result is not order-dependant
                H.m_value += hashValue(value).m_value;
            }
            return H;
        }

        template<typename T>
        concept TRawHashable = details::is_relocatable_v<T> && std::is_scalar_v<T>;

        template<std::ranges::contiguous_range ContiguousRangeT>
        [[nodiscard]] PPR_FORCE_INLINE constexpr hash_t contiguousRange(ContiguousRangeT &&values) noexcept
            requires TRawHashable<std::ranges::range_value_t<ContiguousRangeT> > {
            return memory(std::ranges::data(values),
                          std::ranges::size(values) * sizeof(std::ranges::range_value_t<ContiguousRangeT>),
                          default_seed_v);
        }

        template<std::ranges::sized_range RangeT>
        [[nodiscard]] PPR_FORCE_INLINE constexpr hash_t anyRange(RangeT &&values) noexcept
            requires hash::THashable<std::ranges::range_value_t<RangeT> > ||
                     hash::TRawHashable<std::ranges::range_value_t<RangeT> > {
            if constexpr (std::ranges::contiguous_range<RangeT> && hash::TRawHashable<std::ranges::range_value_t<RangeT> >) {
                return hash::contiguousRange(std::forward<RangeT>(values));
            } else {
                return hash::sizedRange(std::forward<RangeT>(values));
            }
        }
    }

    template<std::ranges::contiguous_range ContiguousRangeT>
    [[nodiscard]] PPR_FORCE_INLINE constexpr hash_t hashValue(ContiguousRangeT &&values) noexcept
        requires hash::TRawHashable<std::ranges::range_value_t<ContiguousRangeT> > {
        return hash::contiguousRange(std::forward<ContiguousRangeT>(values));
    }

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
            concept nothrow_sortable =
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
        ) noexcept(details::nothrow_sortable<IteratorT, CompT, ProjT>) {
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
}
