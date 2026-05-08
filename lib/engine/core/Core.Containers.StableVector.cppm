module;
#include "pP/Macros.h"
export module engine.core:stable_vector;

import :assert;
import :hal;
import :memory;

import std;

export namespace pP {
    // ------------------------------------------------------------------
    // stable vector grows exponentially without invalidating storage
    // ------------------------------------------------------------------

    template<typename T, mem::details::TAllocator AllocatorT = mem::GPA>
    class StableVector;

    namespace details {
        inline constexpr u32 stable_vector_min_capacity = 8u;

        template<typename T, mem::details::TAllocator AllocatorT>
        class StableVectorIterator {
        public:
            using stable_vector = std::conditional_t<
                std::is_const_v<T>,
                std::add_const_t<StableVector<std::remove_const_t<T>, AllocatorT> >,
                StableVector<T, AllocatorT> >;

            using iterator_category = std::random_access_iterator_tag;
            using iterator_concept = std::random_access_iterator_tag; // C++20+

            using value_type = T;
            using pointer = std::add_pointer_t<T>;
            using reference = std::add_lvalue_reference_t<T>;

            using difference_type = std::ptrdiff_t;

        private:
            friend stable_vector;
            template<typename, mem::details::TAllocator>
            friend class StableVectorIterator;

            stable_vector *m_vector{nullptr};

            // fast-path state
            pointer m_slice_ptr{nullptr};
            u32 m_slice_first{0u};
            u32 m_slice_last{0u};

            u32 m_index{umax_v}; // umax_v means end()

            // cold path
            constexpr void initFromIndexFallback_() noexcept {
                if (!m_vector || m_index >= m_vector->m_size) {
                    m_index = umax_v;
                    m_slice_ptr = nullptr;
                    m_slice_first = m_slice_last = 0u;
                    return;
                }

                if (m_vector->m_slices.getTag() == stable_vector::slices_is_single_slice_) [[likely]] {
                    m_slice_ptr = m_vector->m_slices.template getReinterpret<value_type>();
                    m_slice_first = 0u;
                    m_slice_last = m_vector->m_size;
                } else {
                    const u32 slice_index = stable_vector::sliceIndex_(m_index);
                    m_slice_ptr = m_vector->m_slices[slice_index].getData();
                    m_slice_first = stable_vector::sliceOffset_(slice_index);
                    m_slice_last = std::min(m_vector->m_size, m_slice_first + stable_vector::sliceCapacity_(slice_index));
                }
            }

            // hot path
            PPR_FORCE_INLINE constexpr void initFromIndex_() noexcept {
                if (m_index >= m_slice_first && m_index < m_slice_last) [[likely]] {
                    return;
                }
                initFromIndexFallback_();
            }

        public:
            constexpr StableVectorIterator() noexcept = default;

            constexpr StableVectorIterator(const StableVectorIterator &) noexcept = default;

            constexpr StableVectorIterator &operator =(const StableVectorIterator &) noexcept = default;

            constexpr StableVectorIterator(StableVectorIterator &&) noexcept = default;

            constexpr StableVectorIterator &operator =(StableVectorIterator &&) noexcept = default;

            constexpr StableVectorIterator(stable_vector &container, const std::size_t index) noexcept
                : m_vector(std::addressof(container)),
                  m_index(checked_cast<u32>(index)) {
                initFromIndexFallback_();
            }

            constexpr StableVectorIterator(stable_vector &container, const UnsignedMax end) noexcept
                : m_vector(std::addressof(container)),
                  m_index(end) {
                initFromIndexFallback_();
            }

            explicit constexpr StableVectorIterator(const StableVectorIterator<std::remove_const_t<T>, AllocatorT> &other) noexcept
                requires std::is_const_v<T>
                : m_vector(other.m_vector),
                  m_slice_ptr(other.m_slice_ptr),
                  m_slice_first(other.m_slice_first),
                  m_slice_last(other.m_slice_last),
                  m_index(other.m_index) {
            }

            constexpr StableVectorIterator &operator =(const StableVectorIterator<std::remove_const_t<T>, AllocatorT> &other) noexcept
                requires std::is_const_v<T> {
                m_vector = other.m_vector;
                m_slice_ptr = other.m_slice_ptr;
                m_slice_first = other.m_slice_first;
                m_slice_last = other.m_slice_last;
                m_index = other.m_index;
                return (*this);
            }

            [[nodiscard]] PPR_FORCE_INLINE constexpr bool isValid() const noexcept {
                return (m_slice_ptr && m_index >= m_slice_first && m_index < m_slice_last);
            }

            [[nodiscard]] PPR_FORCE_INLINE constexpr u32 getIndex() const noexcept {
                PPR_ASSERT(m_index != umax_v || m_vector);
                return (m_index == umax_v ? m_vector->m_size : m_index);
            }

            [[nodiscard]] PPR_FORCE_INLINE constexpr pointer getPointer() const noexcept {
                PPR_ASSERT(m_slice_ptr && m_index >= m_slice_first && m_index < m_slice_last);
                PPR_ASSUME(m_slice_ptr != nullptr);
                return std::addressof(m_slice_ptr[m_index - m_slice_first]);
            }

            // ------------------------------------------------------------
            // dereference
            // ------------------------------------------------------------
            [[nodiscard]] constexpr reference operator*() const noexcept {
                return *getPointer();
            }

            [[nodiscard]] constexpr pointer operator->() const noexcept {
                return getPointer();
            }

            // ------------------------------------------------------------
            // increment
            // ------------------------------------------------------------
            constexpr StableVectorIterator &operator++() noexcept {
                PPR_ASSERT(m_index != umax_v);
                ++m_index;
                initFromIndex_();
                return *this;
            }

            constexpr StableVectorIterator operator++(int) noexcept {
                auto tmp = *this;
                ++(*this);
                return tmp;
            }

            // ------------------------------------------------------------
            // decrement
            // ------------------------------------------------------------
            constexpr StableVectorIterator &operator--() noexcept {
                if (const u32 actual_index = getIndex(); actual_index > 0u) [[likely]] {
                    m_index = actual_index - 1u;
                } else {
                    m_index = umax_v;
                }
                initFromIndex_();
                return *this;
            }

            constexpr StableVectorIterator operator--(int) noexcept {
                auto tmp = *this;
                --(*this);
                return tmp;
            }

            // ------------------------------------------------------------
            // random access (fallback to index)
            // ------------------------------------------------------------
            constexpr StableVectorIterator &operator+=(const difference_type n) noexcept {
                m_index = checked_cast<u32>(getIndex() + n);
                initFromIndex_(); // fallback: recompute
                return *this;
            }

            constexpr StableVectorIterator &operator-=(const difference_type n) noexcept {
                m_index = checked_cast<u32>(getIndex() - n);
                initFromIndex_();
                return *this;
            }

            [[nodiscard]] friend constexpr StableVectorIterator
            operator+(StableVectorIterator it, difference_type n) noexcept {
                it += n;
                return it;
            }

            [[nodiscard]] friend constexpr StableVectorIterator
            operator+(difference_type n, StableVectorIterator it) noexcept {
                it += n;
                return it;
            }

            [[nodiscard]] friend constexpr StableVectorIterator
            operator-(StableVectorIterator it, difference_type n) noexcept {
                it -= n;
                return it;
            }

            [[nodiscard]] constexpr reference operator[](difference_type n) const noexcept {
                return *(*this + n);
            }

            // ------------------------------------------------------------
            // comparisons
            // ------------------------------------------------------------
            [[nodiscard]] friend constexpr bool
            operator==(const StableVectorIterator &a, const StableVectorIterator &b) noexcept {
                PPR_ASSERT(a.m_vector == b.m_vector);
                return a.getIndex() == b.getIndex();
            }

            [[nodiscard]] friend constexpr std::strong_ordering
            operator<=>(const StableVectorIterator &a, const StableVectorIterator &b) noexcept {
                PPR_ASSERT(a.m_vector == b.m_vector);
                return a.getIndex() <=> b.getIndex();
            }

            // ------------------------------------------------------------
            // distance
            // ------------------------------------------------------------
            [[nodiscard]] friend constexpr difference_type
            operator-(const StableVectorIterator &a, const StableVectorIterator &b) noexcept {
                PPR_ASSERT(a.m_vector == b.m_vector);
                return static_cast<difference_type>(a.getIndex())
                       - static_cast<difference_type>(b.getIndex());
            }
        };
    }

    template<typename T, mem::details::TAllocator AllocatorT = mem::GPA>
    using StableVectorInplace = StableVector<T,
        mem::InSituFallback<details::stable_vector_min_capacity * sizeof(T), AllocatorT, alignof_v<T> > >;

    template<typename T, mem::details::TAllocator AllocatorT>
    class StableVector : mem::Allocator<AllocatorT> {
        friend class details::StableVectorIterator<T, AllocatorT>;
        friend class details::StableVectorIterator<const T, AllocatorT>;

    protected:
        using allocator_type = mem::Allocator<AllocatorT>;
        static constexpr u32 min_capacity_ = details::stable_vector_min_capacity;

        enum ESliceTag_ {
            standalone_slice_ = 0u,
            composite_slice_start_ = 1u,
            composite_slice_middle_ = 2u,
            composite_slice_end_ = 3u,
        };

        using slice_t = TagPtr<T, ESliceTag_>;

        enum ESliceArrayTag_ {
            slices_is_array_ = 0u,
            slices_is_single_slice_ = 1u,
        };

        using slices_array_t = TagPtr<slice_t, ESliceArrayTag_>;

        slices_array_t m_slices{};

        u32 m_capacity{0u};
        u32 m_size{0u};

        [[nodiscard]] PPR_FORCE_INLINE static constexpr u32 sliceIndex_(const u32 item_index) noexcept {
            return static_cast<u32>(std::max(int{3}, std::bit_width(item_index)) - 3);
        }

        [[nodiscard]] PPR_FORCE_INLINE static constexpr u32 sliceOffset_(const u32 slice_index) noexcept {
            return (slice_index ? 1u << (2u + slice_index) : 0u);
        }

        [[nodiscard]] PPR_FORCE_INLINE static constexpr u32 sliceCapacity_(const u32 slice_index) noexcept {
            return 1u << (2u + std::max(1u, slice_index));
        }

        [[nodiscard]] PPR_FORCE_INLINE auto &getAllocator_() noexcept {
            return allocator_type::materialize();
        }

        [[nodiscard]] PPR_FORCE_INLINE const auto &getAllocator_() const noexcept {
            return allocator_type::materialize();
        }

        void expandSingleSliceToArray_(const u32 actual_num_slices, const u32 wanted_num_slices) noexcept {
            PPR_ASSERT(m_slices.getTag() == slices_is_single_slice_);
            PPR_ASSERT(wanted_num_slices > 1u);

            T *const uniq_slice_ptr = m_slices.template getReinterpret<T>();
            slice_t *const slices_arr = allocator_type::template allocate<slice_t>(wanted_num_slices);
            PPR_ASSERT(slices_arr != nullptr);
            PPR_ASSUME(slices_arr != nullptr);

            for (u32 slice_index = 0u; slice_index < actual_num_slices; ++slice_index) {
                const u32 slice_offset = sliceOffset_(slice_index);

                ESliceTag_ slice_tag = standalone_slice_;
                if (actual_num_slices > 1u) {
                    slice_tag = composite_slice_middle_;
                    if (slice_index == 0u) {
                        slice_tag = composite_slice_start_;
                    } else if (slice_index == actual_num_slices - 1u) {
                        slice_tag = composite_slice_end_;
                    }
                }

                slices_arr[slice_index].reset(uniq_slice_ptr + slice_offset, slice_tag);
            }

            m_slices.reset(slices_arr, slices_is_array_);
        }

        void resetSliceArray_() noexcept {
            PPR_ASSERT(m_slices.getTag() == slices_is_array_);
            const u32 num_slices = sliceIndex_(m_capacity);

            T *composite_slice_ptr = nullptr;
            std::size_t composite_slice_size = 0u;
            for (u32 i = 0u; i < num_slices; i++) {
                PPR_ASSERT(m_slices[i].isValid());
                const u32 slice_capacity = sliceCapacity_(i);

                if constexpr (!std::is_trivially_destructible_v<T>) {
                    if (const u32 slice_offset = sliceOffset_(i); slice_offset < m_size) {
                        const u32 num_to_destroy = std::min(slice_capacity, m_size - slice_offset);
                        std::destroy_n(m_slices[i].getData(), num_to_destroy);
                    }
                }

                switch (const slice_t slice = m_slices[i]; slice.getTag()) {
                    case standalone_slice_: // standalone
                        allocator_type::template deallocate<T>(slice.getData(), slice_capacity);
                        break;

                    case composite_slice_start_:
                        PPR_ASSERT(composite_slice_ptr == nullptr && composite_slice_size == 0u);

                        composite_slice_ptr = slice.getData();
                        composite_slice_size += slice_capacity;
                        break;

                    case composite_slice_middle_:
                        PPR_ASSERT(composite_slice_ptr && composite_slice_size > 0u);
                        PPR_ASSERT(composite_slice_ptr + composite_slice_size == slice.getData());

                        composite_slice_size += slice_capacity;
                        break;

                    case composite_slice_end_:
                        PPR_ASSERT(composite_slice_ptr + composite_slice_size == slice.getData());

                        allocator_type::template deallocate<T>(composite_slice_ptr, composite_slice_size + slice_capacity);
                        composite_slice_ptr = nullptr;
                        composite_slice_size = 0u;
                        break;

                    default: std::unreachable();
                }
            }

            allocator_type::template deallocate<slice_t>(m_slices.getData(), num_slices);
            m_slices = {};
        }

    public:
        using value_type = T;
        using pointer = std::add_pointer_t<T>;
        using reference = std::add_lvalue_reference_t<T>;
        using size_type = std::size_t;

        using iterator = details::StableVectorIterator<T, AllocatorT>;
        using const_iterator = details::StableVectorIterator<const T, AllocatorT>;

        static_assert(std::random_access_iterator<iterator>);
        static_assert(std::random_access_iterator<const_iterator>);

        constexpr StableVector() noexcept
            requires std::is_default_constructible_v<allocator_type> = default;

        explicit constexpr StableVector(const std::size_t initial_capacity) noexcept
            requires std::is_default_constructible_v<allocator_type> {
            reserveAssumeEmpty(initial_capacity);
        }

        constexpr StableVector(std::initializer_list<T> init_values) noexcept
            requires std::is_default_constructible_v<allocator_type> {
            assignAssumeEmpty(init_values);
        }

        constexpr StableVector(const std::size_t size, const T &init_value) noexcept
            requires std::is_default_constructible_v<allocator_type> {
            resize(size, init_value);
        }

        explicit constexpr StableVector(AllocatorT &&al) noexcept
            : allocator_type(std::move(al)) {
        }

        explicit constexpr StableVector(const AllocatorT &al) noexcept
            : allocator_type(al) {
        }

        constexpr StableVector(const std::size_t initial_capacity, AllocatorT &&al) noexcept
            : allocator_type(std::move(al)) {
            reserveAssumeEmpty(initial_capacity);
        }

        constexpr StableVector(const std::size_t initial_capacity, const AllocatorT &al) noexcept
            : allocator_type(al) {
            reserveAssumeEmpty(initial_capacity);
        }

        constexpr StableVector(std::initializer_list<T> init_values, AllocatorT &&al) noexcept
            : allocator_type(std::move(al)) {
            assignAssumeEmpty(init_values);
        }

        constexpr StableVector(std::initializer_list<T> init_values, const AllocatorT &al) noexcept
            : allocator_type(al) {
            assignAssumeEmpty(init_values);
        }

        constexpr StableVector(const std::size_t size, const T &init_value, AllocatorT &&al) noexcept
            : allocator_type(std::move(al)) {
            resize(size, init_value);
        }

        constexpr StableVector(const std::size_t size, const T &init_value, const AllocatorT &al) noexcept
            : allocator_type(al) {
            resize(size, init_value);
        }

        constexpr StableVector(const StableVector &other) noexcept
            : allocator_type(other) {
            assignAssumeEmpty(other);
        }

        constexpr StableVector &operator =(const StableVector &other) noexcept {
            if (this == &other) [[unlikely]] {
                return *this;
            }

            reset();
            allocator_type::operator=(other);
            assignAssumeEmpty(other);
            return *this;
        }

        constexpr StableVector(StableVector &&rvalue) noexcept
            : allocator_type(std::move(rvalue)) {
            spliceAssumeEmpty(rvalue);
        }

        constexpr StableVector &operator =(StableVector &&rvalue) noexcept {
            if (this == &rvalue) [[unlikely]] {
                return *this;
            }

            reset();
            allocator_type::operator=(std::move(rvalue));
            spliceAssumeEmpty(rvalue);
            return *this;
        }

        constexpr ~StableVector() noexcept {
            reset();
        }

        [[nodiscard]] PPR_FORCE_INLINE constexpr std::size_t capacity() const noexcept { return m_capacity; }
        [[nodiscard]] PPR_FORCE_INLINE constexpr std::size_t size() const noexcept { return m_size; }
        [[nodiscard]] PPR_FORCE_INLINE constexpr bool isEmpty() const noexcept { return m_size == 0u; }

        [[nodiscard]] PPR_FORCE_INLINE constexpr auto &front(this auto &&self) noexcept { return self.at(0u); }
        [[nodiscard]] PPR_FORCE_INLINE constexpr auto &back(this auto &&self) noexcept { return self.at(self.m_size - 1u); }

        [[nodiscard]] constexpr T &at(const std::size_t index) noexcept {
            PPR_ASSERT(index < m_size);
            if (m_slices.getTag() == slices_is_single_slice_) [[likely]] {
                PPR_ASSERT(m_slices.getData());
                return m_slices.template getReinterpret<T>()[index];
            }
            const u32 slice_index = sliceIndex_(static_cast<u32>(index));
            const u32 rel_index = static_cast<u32>(index) - sliceOffset_(slice_index);
            return m_slices[slice_index][rel_index];
        }

        [[nodiscard]] constexpr const T &at(const std::size_t index) const noexcept {
            return const_cast<StableVector *>(this)->at(index);
        }

        [[nodiscard]] PPR_FORCE_INLINE constexpr auto &
        operator [](this auto &&self, const std::size_t index) noexcept {
            return self.at(index);
        }

        [[nodiscard]] constexpr iterator begin() noexcept { return iterator(*this, 0u); }
        [[nodiscard]] constexpr iterator end() noexcept { return iterator(*this, umax_v); }

        [[nodiscard]] constexpr const_iterator begin() const noexcept { return const_iterator(*this, 0u); }
        [[nodiscard]] constexpr const_iterator end() const noexcept { return const_iterator(*this, umax_v); }

        [[nodiscard]] constexpr const_iterator cbegin() const noexcept { return begin(); }
        [[nodiscard]] constexpr const_iterator cend() const noexcept { return end(); }

        [[nodiscard]] constexpr auto each(this auto &&self) noexcept {
            return std::ranges::subrange(self.begin(), self.end());
        }

        template<std::ranges::range RangeT>
        constexpr void assign(RangeT &&values)
            noexcept(std::is_nothrow_copy_constructible_v<T>)
            requires std::is_convertible_v<std::ranges::range_value_t<RangeT>, T> {
            clear();
            assignAssumeEmpty(std::forward<RangeT>(values));
        }

        template<std::ranges::range RangeT>
        constexpr void assignAssumeEmpty(RangeT &&values)
            noexcept(std::is_nothrow_copy_constructible_v<T>)
            requires std::is_convertible_v<std::ranges::range_value_t<RangeT>, T> {
            const std::size_t n = std::ranges::size(values);
            reserveAssumeEmpty(n);
            m_size = checked_cast<u32>(n);
            std::ranges::uninitialized_copy(values, each());
        }

        constexpr void clear() noexcept(std::is_nothrow_destructible_v<T>) {
            if constexpr (!std::is_trivially_destructible_v<T>) {
                std::ranges::destroy(each());
            }
            m_size = 0u;
        }

        constexpr void shrinkToFit() noexcept {
            if (m_size == 0u) [[unlikely]] {
                reset();
                return;
            }
            const u32 n = std::max(min_capacity_, std::bit_ceil(m_size));
            if (n >= m_capacity || m_slices.getTag() == slices_is_single_slice_) [[likely]] {
                return;
            }
            PPR_ASSERT(m_slices.getTag() == slices_is_array_);

            const u32 actual_num_slices = sliceIndex_(m_capacity);
            u32 wanted_num_slices = sliceIndex_(n);
            PPR_ASSERT(wanted_num_slices < actual_num_slices);

            T *composite_slice_ptr = nullptr;
            u32 composite_slice_size = 0u;
            u32 composite_restriction_on_num_slices = wanted_num_slices;

            for (u32 i = wanted_num_slices; i < actual_num_slices; ++i) {
                PPR_ASSERT(sliceOffset_(i) >= m_size);
                const u32 slice_capacity = sliceCapacity_(i);

                switch (const slice_t slice = m_slices[i]; slice.getTag()) {
                    case standalone_slice_:
                        allocator_type::template deallocate<T>(slice.getData(), slice_capacity);
                        break;

                    case composite_slice_start_:
                        PPR_ASSERT(composite_slice_ptr == nullptr);

                        composite_slice_ptr = slice.getData();
                        composite_slice_size += slice_capacity;
                        break;

                    case composite_slice_middle_:
                        if (composite_slice_ptr == nullptr) {
                            composite_restriction_on_num_slices = std::min(composite_restriction_on_num_slices, i + 1u);
                        } else {
                            PPR_ASSERT(composite_slice_ptr + composite_slice_size == slice.getData());

                            composite_slice_size += slice_capacity;
                        }
                        break;

                    case composite_slice_end_:
                        if (composite_slice_ptr == nullptr) {
                            composite_restriction_on_num_slices = std::min(composite_restriction_on_num_slices, i + 1u);
                        } else {
                            PPR_ASSERT(composite_slice_ptr + composite_slice_size == slice.getData());

                            allocator_type::template deallocate<T>(composite_slice_ptr, composite_slice_size + slice_capacity);
                            composite_slice_ptr = nullptr;
                            composite_slice_size = 0u;
                        }
                        break;

                    default: std::unreachable();
                }

                m_slices[i] = {};
            }
            PPR_ASSERT(composite_slice_ptr == nullptr && composite_slice_size == 0u);

            if (composite_restriction_on_num_slices != wanted_num_slices) [[unlikely]] {
                PPR_ASSERT(composite_restriction_on_num_slices > wanted_num_slices);
                wanted_num_slices = composite_restriction_on_num_slices;
            }

            slice_t *const slice_arr = allocator_type::relocate(m_slices.getData(), actual_num_slices, wanted_num_slices).ptr;
            PPR_ASSERT(slice_arr != nullptr);
            m_slices.reset(slice_arr, slices_is_array_);
            m_capacity = n;
        }

        PPR_FORCE_INLINE constexpr void reserveAdditional(const std::size_t n) noexcept {
            if (m_size + n > m_capacity) [[unlikely]] {
                reserve(m_size + n);
            }
        }

        constexpr void reserve(const std::size_t wanted_capacity) noexcept {
            const u32 n = std::max(min_capacity_, std::bit_ceil(checked_cast<u32>(wanted_capacity)));
            if (n <= m_capacity) [[likely]] {
                return;
            }
            if (m_capacity == 0u) [[unlikely]] {
                reserveAssumeEmpty(n);
                return;
            }

            const u32 actual_num_slices = sliceIndex_(m_capacity);
            const u32 wanted_num_slices = sliceIndex_(n);
            PPR_ASSERT(actual_num_slices < wanted_num_slices);

            if (m_slices.getTag() == slices_is_single_slice_) {
                T *const uniq_slice_ptr = m_slices.template getReinterpret<T>();
                if constexpr (mem::details::TResizableAllocator<AllocatorT>) {
                    if (allocator_type::resize(uniq_slice_ptr, m_capacity, n)) {
                        m_capacity = n;
                        return;
                    }
                }

                expandSingleSliceToArray_(actual_num_slices, wanted_num_slices);
            } else {
                slice_t *const slice_arr = allocator_type::relocate(m_slices.getData(), actual_num_slices, wanted_num_slices).ptr;
                m_slices.reset(slice_arr, slices_is_array_);
            }
            PPR_ASSERT(m_slices.isValid() && m_slices.getTag() == slices_is_array_);

            const u32 new_num_slices = wanted_num_slices - actual_num_slices;
            const u32 total_new_capacity = (1u << (wanted_num_slices + 2u)) - (1u << (actual_num_slices + 2u));
            T *const composite_slice_ptr = allocator_type::template allocate<T>(total_new_capacity);
            PPR_ASSERT(composite_slice_ptr != nullptr);
            u32 composite_slice_size = 0u;

            for (u32 i = actual_num_slices; i < wanted_num_slices; ++i) {
                const u32 slice_capacity = sliceCapacity_(i);
                ESliceTag_ slice_tag = standalone_slice_;
                if (new_num_slices > 1u) {
                    slice_tag = composite_slice_middle_;
                    if (composite_slice_size == 0u) {
                        slice_tag = composite_slice_start_;
                    } else if (composite_slice_size + slice_capacity == total_new_capacity) {
                        slice_tag = composite_slice_end_;
                    }
                }

                m_slices[i].reset(composite_slice_ptr + composite_slice_size, slice_tag);
                composite_slice_size += slice_capacity;
            }
            PPR_ASSERT(composite_slice_size == total_new_capacity);

            m_capacity += total_new_capacity;
        }

        constexpr void reserveAssumeEmpty(const std::size_t wanted_capacity) noexcept {
            PPR_ASSERT(m_capacity == 0u && m_slices.isNull());
            if (wanted_capacity == 0u) [[unlikely]] {
                return;
            }

            m_capacity = std::max(min_capacity_, std::bit_ceil(checked_cast<u32>(wanted_capacity)));

            T *const composite_slice_ptr = allocator_type::template allocate<T>(m_capacity);
            PPR_ASSERT(composite_slice_ptr != nullptr);

            m_slices.reset(reinterpret_cast<slice_t *>(composite_slice_ptr), slices_is_single_slice_);
        }

        constexpr void reset() noexcept(std::is_nothrow_destructible_v<T>) {
            if (m_slices.isNull()) {
                PPR_ASSERT(m_capacity == 0u && m_size == 0u);
                return;
            }

            if (m_slices.getTag() == slices_is_single_slice_) [[likely]] {
                if constexpr (!std::is_trivially_destructible_v<T>) {
                    std::destroy_n(m_slices.template getReinterpret<T>(), std::min(m_size, m_capacity));
                }

                allocator_type::template deallocate<T>(m_slices.template getReinterpret<T>(), m_capacity);
            } else {
                resetSliceArray_();
            }

            m_slices = {};
            m_capacity = 0u;
            m_size = 0u;
        }

        constexpr void resize(const std::size_t new_size, const T &init_value = default_value_v) noexcept {
            if (new_size < m_size) {
                std::ranges::destroy(begin() + static_cast<std::ptrdiff_t>(new_size), end());
                m_size = checked_cast<u32>(new_size);
                shrinkToFit();
            } else if (new_size > m_size) {
                reserve(new_size);
                const u32 old_size = m_size;
                m_size = checked_cast<u32>(new_size);
                std::ranges::uninitialized_fill(begin() + old_size, end(), init_value);
            }
        }

        constexpr void spliceAssumeEmpty(StableVector &src) noexcept {
            PPR_ASSERT(this != &src);
            PPR_ASSERT(m_slices == nullptr);

            m_slices = src.m_slices;
            m_capacity = src.m_capacity;
            m_size = src.m_size;

            src.m_slices = {};
            src.m_capacity = 0;
            src.m_size = 0;
        }

        [[nodiscard]] constexpr T *pushBackUninitialized() noexcept {
            reserveAdditional(1u);
            return std::addressof(at(m_size++));
        }

        [[nodiscard]] constexpr T *pushBackUninitializedAssumeCapacity() noexcept {
            PPR_ASSERT(m_size < m_capacity);
            return std::addressof(at(m_size++));
        }

        constexpr void pushBack(T &&rvalue) noexcept(std::is_nothrow_move_constructible_v<T>)
            requires std::is_move_constructible_v<T> {
            reserveAdditional(1u);
            pushBackAssumeCapacity(std::move(rvalue));
        }

        constexpr void pushBackAssumeCapacity(T &&rvalue) noexcept(std::is_nothrow_move_constructible_v<T>)
            requires std::is_move_constructible_v<T> {
            PPR_ASSERT(m_size < m_capacity);
            std::construct_at(std::addressof(at(m_size++)), std::move(rvalue));
        }

        [[maybe_unused]] constexpr T &pushBackDefault() noexcept(std::is_nothrow_default_constructible_v<T>)
            requires std::is_default_constructible_v<T> {
            reserveAdditional(1u);
            return pushBackDefaultAssumeCapacity();
        }

        [[maybe_unused]] constexpr T &pushBackDefaultAssumeCapacity() noexcept(std::is_nothrow_default_constructible_v<T>)
            requires std::is_default_constructible_v<T> {
            PPR_ASSERT(m_size < m_capacity);
            return *std::construct_at(std::addressof(at(m_size++)));
        }

        constexpr void pushBack(const T &value) noexcept(std::is_nothrow_copy_constructible_v<T>)
            requires std::is_copy_constructible_v<T> {
            reserveAdditional(1u);
            pushBackAssumeCapacity(value);
        }

        constexpr void pushBackAssumeCapacity(const T &value) noexcept(std::is_nothrow_copy_constructible_v<T>)
            requires std::is_copy_constructible_v<T> {
            PPR_ASSERT(m_size < m_capacity);
            std::construct_at(std::addressof(at(m_size++)), value);
        }

        template<typename... ArgsT>
        [[maybe_unused]] constexpr T &emplaceBack(ArgsT &&... args)
            noexcept(std::is_nothrow_constructible_v<T, ArgsT &&...>)
            requires std::is_constructible_v<T, ArgsT &&...> {
            reserveAdditional(1u);
            return emplaceBackAssumeCapacity(std::forward<ArgsT>(args)...);
        }

        template<typename... ArgsT>
        [[maybe_unused]] constexpr T &emplaceBackAssumeCapacity(ArgsT &&... args)
            noexcept(std::is_nothrow_constructible_v<T, ArgsT &&...>)
            requires std::is_constructible_v<T, ArgsT &&...> {
            PPR_ASSERT(m_size < m_capacity);
            return *std::construct_at(std::addressof(at(m_size++)), std::forward<ArgsT>(args)...);
        }

        constexpr void popBack() noexcept(std::is_nothrow_destructible_v<T>) {
            PPR_ASSERT(m_size > 0u);
            std::destroy_at(std::addressof(at(--m_size)));
        }

        template<typename ValueT>
        [[nodiscard]] constexpr auto
        find(this auto &&self, ValueT &&value_to_find) noexcept
            requires std::equality_comparable_with<T, ValueT> {
            return std::find(self.begin(), self.end(), std::forward<ValueT>(value_to_find));
        }

        template<std::ranges::range RangeT>
        [[maybe_unused]] constexpr std::size_t
        append(RangeT &&range) noexcept(std::is_nothrow_copy_constructible_v<T>)
            requires std::is_convertible_v<std::ranges::range_value_t<RangeT>, T> {
            const std::size_t n = std::ranges::size(range);
            reserveAdditional(n);
            return appendAssumeCapacity(std::ranges::begin(range), std::ranges::end(range));
        }

        template<std::forward_iterator IteratorT> requires details::is_iterator_of<IteratorT, T>
        [[maybe_unused]] constexpr std::size_t
        append(IteratorT first, IteratorT last) noexcept(std::is_nothrow_copy_constructible_v<T>) {
            if constexpr (std::random_access_iterator<IteratorT>) {
                const std::size_t n = static_cast<std::size_t>(std::distance(first, last));
                reserveAdditional(n);
                for (; first != last; ++first) {
                    pushBackAssumeCapacity(*first);
                }
                return n;
            } else {
                std::size_t num_append = 0u;
                for (; first != last; ++first, ++num_append) {
                    pushBack(*first);
                }
                return num_append;
            }
        }

        template<std::forward_iterator IteratorT> requires details::is_iterator_of<IteratorT, T>
        [[maybe_unused]] constexpr std::size_t
        appendAssumeCapacity(IteratorT first, IteratorT last) noexcept(std::is_nothrow_copy_constructible_v<T>) {
            std::size_t num_append = 0u;
            for (; first != last; ++first, ++num_append) {
                pushBackAssumeCapacity(*first);
            }
            return num_append;
        }

        constexpr void insert(const const_iterator &where, T &&rvalue) noexcept {
            PPR_ASSERT(where.m_vector == this);
            insert(where.m_index, std::move(rvalue));
        }

        constexpr void insert(const const_iterator &where, const T &value) noexcept {
            PPR_ASSERT(where.m_vector == this);
            insert(where.m_index, value);
        }

        constexpr void insert(const std::size_t index, T &&rvalue) noexcept {
            PPR_ASSERT(index <= m_size);
            pushBack(std::move(rvalue));
            const auto range = each();
            std::ranges::rotate(range.begin() + index, range.end() - 1u, range.end());
        }

        constexpr void insert(const std::size_t index, const T &value) noexcept {
            PPR_ASSERT(index <= m_size);
            pushBack(value);
            const auto range = each();
            std::ranges::rotate(range.begin() + index, range.end() - 1u, range.end());
        }

        template<std::ranges::range RangeT>
        constexpr void insert(const const_iterator &where, RangeT &&range) noexcept
            requires std::is_convertible_v<std::ranges::range_value_t<RangeT>, T> {
            insert(where, std::ranges::begin(range), std::ranges::end(range));
        }

        template<std::ranges::range RangeT>
        constexpr void insert(const std::size_t index, RangeT &&range) noexcept
            requires std::is_convertible_v<std::ranges::range_value_t<RangeT>, T> {
            insert(index, std::ranges::begin(range), std::ranges::end(range));
        }

        template<std::forward_iterator IteratorT> requires details::is_iterator_of<IteratorT, T>
        constexpr void insert(const const_iterator &where, IteratorT first, IteratorT last) noexcept {
            PPR_ASSERT(where.m_vector == this);
            insert(where.m_index, first, last);
        }

        template<std::forward_iterator IteratorT> requires details::is_iterator_of<IteratorT, T>
        constexpr void insert(const std::size_t index, IteratorT first, IteratorT last) noexcept {
            PPR_ASSERT(index <= m_size);
            const std::size_t num_append = append(first, last);
            if (num_append > 0) {
                const auto range = each();
                std::ranges::rotate(std::ranges::begin(range) + static_cast<std::ptrdiff_t>(index),
                                    std::ranges::end(range) - static_cast<std::ptrdiff_t>(num_append),
                                    std::ranges::end(range));
            }
        }

        constexpr iterator erase(const const_iterator &it) noexcept(std::is_nothrow_destructible_v<T>) {
            PPR_ASSERT(it.m_vector == this);
            erase(it.m_index);
            return iterator(*this, it.m_index);
        }

        constexpr void erase(const std::size_t index) noexcept(std::is_nothrow_destructible_v<T>) {
            PPR_ASSERT(index < m_size);
            const auto range = each();
            if (index + 1u < m_size) {
                std::ranges::rotate(std::ranges::begin(range) + static_cast<std::ptrdiff_t>(index), std::ranges::begin(range) + static_cast<std::ptrdiff_t>(index) + 1,
                                    std::ranges::end(range));
            }
            std::destroy_at(std::addressof(at(m_size - 1u)));
            m_size--;
        }

        constexpr iterator eraseSwapBack(const const_iterator &it) noexcept(std::is_nothrow_destructible_v<T>) {
            PPR_ASSERT(it.m_vector == this);
            eraseSwapBack(it.m_index);
            return iterator(*this, it.m_index);
        }

        constexpr void eraseSwapBack(const std::size_t index) noexcept(std::is_nothrow_destructible_v<T>) {
            PPR_ASSERT(index < m_size);
            if (index + 1u < m_size) {
                std::swap(back(), at(index));
            }
            std::destroy_at(std::addressof(back()));
            m_size--;
        }

        template<typename ValueT>
        constexpr void eraseAllSwapBack(ValueT &&value_to_erase)
            noexcept(std::is_nothrow_destructible_v<T> && std::is_nothrow_move_constructible_v<T>)
            requires std::equality_comparable_with<T, ValueT> {
            for (iterator it = begin(); it != end();) {
                if (*it == value_to_erase) {
                    it = eraseSwapBack(it);
                } else {
                    ++it;
                }
            }
        }

        template<typename OtherAllocatorT>
        friend bool operator ==(const StableVector &lhs, const StableVector<T, OtherAllocatorT> &rhs) noexcept {
            return std::ranges::equal(lhs, rhs);
        }

        friend void swap(StableVector &lhs, StableVector &rhs) noexcept {
            using namespace std;
            swap(static_cast<allocator_type &>(lhs), static_cast<allocator_type &>(rhs));
            swap(lhs.m_slices, rhs.m_slices);
            swap(lhs.m_capacity, rhs.m_capacity);
            swap(lhs.m_size, rhs.m_size);
        }
    };

    template<typename T>
    StableVector(std::initializer_list<T>) -> StableVector<std::remove_cvref_t<T> >;

    template<typename T>
    StableVector(std::size_t n, T &&rvalue) -> StableVector<std::remove_cvref_t<T> >;

    template<typename T>
    StableVector(std::size_t n, const T &value) -> StableVector<std::remove_cvref_t<T> >;

    template<typename T, mem::details::TAllocator AllocatorT>
    StableVector(std::initializer_list<T>, AllocatorT &&al) -> StableVector<std::remove_cvref_t<T>, std::remove_cvref_t<AllocatorT> >;

    template<typename T, mem::details::TAllocator AllocatorT>
    StableVector(std::initializer_list<T>, const AllocatorT &al) -> StableVector<std::remove_cvref_t<T>, std::remove_cvref_t<AllocatorT> >;

    template<typename T, mem::details::TAllocator AllocatorT>
    StableVector(std::size_t n, T &&rvalue, AllocatorT &&al) -> StableVector<std::remove_cvref_t<T>, std::remove_cvref_t<AllocatorT> >;

    template<typename T, mem::details::TAllocator AllocatorT>
    StableVector(std::size_t n, T &&rvalue, const AllocatorT &al) -> StableVector<std::remove_cvref_t<T>, std::remove_cvref_t<AllocatorT> >;

    template<typename T, mem::details::TAllocator AllocatorT>
    StableVector(std::size_t n, const T &value, AllocatorT &&al) -> StableVector<std::remove_cvref_t<T>, std::remove_cvref_t<AllocatorT> >;

    template<typename T, mem::details::TAllocator AllocatorT>
    StableVector(std::size_t n, const T &value, const AllocatorT &al) -> StableVector<std::remove_cvref_t<T>, std::remove_cvref_t<AllocatorT> >;

    template<typename T, mem::details::TAllocator AllocatorT>
    struct details::relocatable<StableVector<T, AllocatorT> > : relocatable<mem::Allocator<AllocatorT> > {
    };
}
