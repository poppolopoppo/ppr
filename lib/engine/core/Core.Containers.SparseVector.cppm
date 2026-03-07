module;
#include "pP/Macros.h"
export module engine.core:sparse_vector;

import :assert;
import :containers;
import :hal;
import :memory;
import :stable_vector;

import std;

export namespace pP {
    // ------------------------------------------------------------------
    // sparse vector is a stable vector which never shrinks and uses a free-list
    // ------------------------------------------------------------------

    template<typename T, mem::details::TAllocator AllocatorT = mem::GPA>
    class SparseVector;

    // ------------------------------------------------------------------
    // sparse vector wraps the values with a payload to handle free-list, jump-counting and lifetime validation
    // ------------------------------------------------------------------

    namespace details {
        struct SparseVectorPayload {
            u32 m_skip: 24 = 0u;
            u32 m_seed: 8 = 0u;
        };

        template<typename T>
        struct alignas(T) SparseVectorItem {
            struct FreeList { // NOLINT(*-pro-type-member-init)
                u32 m_next_free;
            };

            union {
                alignas(T) FreeList m_free_list{};
                alignas(T) std::byte m_storage[sizeof(T)];
            };

            SparseVectorPayload m_payload;

            [[nodiscard]] PPR_FORCE_INLINE constexpr T *getValuePtr() noexcept {
                return reinterpret_cast<T *>(&m_storage);
            }

            [[nodiscard]] PPR_FORCE_INLINE constexpr const T *getValuePtr() const noexcept {
                return reinterpret_cast<const T *>(&m_storage);
            }
        };
    }

    // ------------------------------------------------------------------
    // sparse vector key, with a seed acting as a generation validator for lifetime management
    // ------------------------------------------------------------------

    struct SparseKeyId {
        u32 m_index: 24 = 0xFFFFFFu;
        u32 m_seed: 8 = 0xFFu;

        constexpr SparseKeyId() noexcept = default;

        constexpr SparseKeyId(const details::SparseVectorPayload &payload, const u32 index) noexcept
            : m_index(index), m_seed(payload.m_seed) {
        }

        [[nodiscard]] PPR_FORCE_INLINE constexpr bool isValid() const noexcept {
            return m_seed != 0u;
        }

        [[nodiscard]] constexpr bool operator ==(const SparseKeyId &other) const noexcept = default;

        [[nodiscard]] constexpr std::strong_ordering operator <=>(const SparseKeyId other) const noexcept {
            if (isValid() && other.isValid()) [[likely]] {
                return m_index <=> other.m_index;
            }
            return isValid() ? std::strong_ordering::less : (other.isValid() ? std::strong_ordering::greater : std::strong_ordering::equal);
        }

        [[nodiscard]] PPR_FORCE_INLINE friend constexpr hash_t hashValue(const SparseKeyId key) noexcept {
            return hashValue(std::bit_cast<u32>(key));
        }
    };

    // ------------------------------------------------------------------
    // sparse vector iterator
    // ------------------------------------------------------------------

    namespace details {
        template<typename T, mem::details::TAllocator AllocatorT>
        class SparseVectorIterator {
        public:
            using sparse_vector = std::conditional_t<
                std::is_const_v<T>,
                std::add_const_t<SparseVector<std::remove_const_t<T>, AllocatorT> >,
                SparseVector<T, AllocatorT> >;
            friend sparse_vector;

            using iterator_category = std::bidirectional_iterator_tag;
            using iterator_concept = std::bidirectional_iterator_tag; // C++20+

            using value_type = T;
            using difference_type = std::ptrdiff_t;
            using pointer = T *;
            using reference = T &;

        private:
            template<typename, mem::details::TAllocator>
            friend class SparseVectorIterator;

            using stable_vector = std::conditional_t<
                std::is_const_v<T>,
                std::add_const_t<typename sparse_vector::stable_vector>,
                typename sparse_vector::stable_vector>;
            using stable_vector_iterator = std::conditional_t<
                std::is_const_v<T>,
                typename stable_vector::const_iterator,
                typename stable_vector::iterator>;

            stable_vector_iterator m_stable_it{};

            explicit constexpr SparseVectorIterator(const stable_vector_iterator &stable_it) noexcept
                : m_stable_it(stable_it) {
            }

        public:
            constexpr SparseVectorIterator() noexcept = default;

            constexpr SparseVectorIterator(const SparseVectorIterator &) noexcept = default;

            constexpr SparseVectorIterator &operator =(const SparseVectorIterator &) noexcept = default;

            constexpr SparseVectorIterator(SparseVectorIterator &&) noexcept = default;

            constexpr SparseVectorIterator &operator =(SparseVectorIterator &&) noexcept = default;

            constexpr SparseVectorIterator(sparse_vector &container, const std::size_t index) noexcept
                : m_stable_it(static_cast<stable_vector &>(container), index) {
            }

            constexpr SparseVectorIterator(sparse_vector &container, const UnsignedMax end) noexcept
                : m_stable_it(static_cast<stable_vector &>(container), end) {
            }

            constexpr SparseVectorIterator(const SparseVectorIterator<std::remove_const_t<T>, AllocatorT> &other) noexcept
                requires std::is_const_v<T>
                : m_stable_it(other.m_stable_it) {
            }

            constexpr SparseVectorIterator &operator =(const SparseVectorIterator<std::remove_const_t<T>, AllocatorT> &other) noexcept
                requires std::is_const_v<T> {
                m_stable_it = other.m_stable_it;
                return (*this);
            }


            [[nodiscard]] PPR_FORCE_INLINE constexpr bool isValid() const noexcept {
                return m_stable_it.isValid();
            }

            [[nodiscard]] PPR_FORCE_INLINE constexpr SparseKeyId getKey() const noexcept {
                if (isValid()) [[likely]] {
                    return SparseKeyId(m_stable_it->m_payload, m_stable_it.getIndex());
                }
                return default_value_v; // invalid key
            }

            [[nodiscard]] PPR_FORCE_INLINE constexpr u32 getIndex() const noexcept {
                return m_stable_it.getIndex();
            }

            [[nodiscard]] PPR_FORCE_INLINE constexpr pointer getPointer() const noexcept {
                return m_stable_it->getValuePtr();
            }

            // ------------------------------------------------------------
            // dereference
            // ------------------------------------------------------------
            [[nodiscard]] constexpr reference operator*() const noexcept {
                return (*m_stable_it->getValuePtr());
            }

            [[nodiscard]] constexpr pointer operator->() const noexcept {
                return m_stable_it->getValuePtr();
            }

            // ------------------------------------------------------------
            // increment
            // ------------------------------------------------------------
            constexpr SparseVectorIterator &operator++() noexcept {
                if ((++m_stable_it).isValid()) {
                    if (const u32 skip_dist = m_stable_it->m_payload.m_skip) {
                        m_stable_it += skip_dist;
                    }
                }
                return *this;
            }

            constexpr SparseVectorIterator operator++(int) noexcept {
                auto tmp = *this;
                ++(*this);
                return tmp;
            }

            // ------------------------------------------------------------
            // decrement
            // ------------------------------------------------------------
            constexpr SparseVectorIterator &operator--() noexcept {
                if ((--m_stable_it).isValid()) {
                    if (const u32 skip_dist = m_stable_it->m_payload.m_skip) {
                        m_stable_it -= skip_dist;
                    }
                }
                return *this;
            }

            constexpr SparseVectorIterator operator--(int) noexcept {
                auto tmp = *this;
                --(*this);
                return tmp;
            }

            // ------------------------------------------------------------
            // comparisons
            // ------------------------------------------------------------
            [[nodiscard]] friend constexpr bool
            operator==(const SparseVectorIterator &a, const SparseVectorIterator &b) noexcept {
                return (a.m_stable_it == b.m_stable_it);
            }

            [[nodiscard]] friend constexpr std::strong_ordering
            operator<=>(const SparseVectorIterator &a, const SparseVectorIterator &b) noexcept {
                return (a.m_stable_it <=> b.m_stable_it);
            }
        };
    }

    // ------------------------------------------------------------------
    // sparse vector variant with in-situ storage for the first slice (8 elements)
    // ------------------------------------------------------------------

    template<typename T, mem::details::TAllocator AllocatorT = mem::GPA>
    using SparseVectorInplace = SparseVector<T,
        mem::InSituFallback<details::stable_vector_min_capacity * sizeof(details::SparseVectorItem<T>),
            AllocatorT, alignof_v<details::SparseVectorItem<T> > > >;

    // ------------------------------------------------------------------
    // general purpose sparse vector with stable pointers and generation keys to validate lifetime
    // ------------------------------------------------------------------

    template<typename T, mem::details::TAllocator AllocatorT>
    class SparseVector : protected StableVector<details::SparseVectorItem<T>, AllocatorT> {
        friend class details::SparseVectorIterator<T, AllocatorT>;
        friend class details::SparseVectorIterator<const T, AllocatorT>;

        using stable_vector = StableVector<details::SparseVectorItem<T>, AllocatorT>;
        using allocator_type = stable_vector::allocator_type;
        using stable_vector_iterator = stable_vector::iterator;
        using sparse_vector_item = details::SparseVectorItem<T>;

        // cyclic single linked list using indices instead of pointers
        u32 m_free_tail = none_v;

        u32 m_size_alive: 24 = 0u;
        u32 m_seed_alloc: 8 = 0u;

        [[nodiscard]] constexpr u32 nextAllocationSeed_() noexcept {
            if (m_seed_alloc == 0u) [[unlikely]] {
                m_seed_alloc = static_cast<u32>(hash::mix(stable_vector::m_slices.m_packed));
            }
            m_seed_alloc = std::max(1u, (m_seed_alloc + 1u) & 0xFFu);
            PPR_ASSERT(m_seed_alloc);
            return m_seed_alloc;
        }

        [[nodiscard]] constexpr u32 freePopAssumeNotEmpty_() noexcept {
            PPR_ASSERT(m_free_tail != none_v);
            sparse_vector_item &tail = stable_vector::at(m_free_tail);
            const u32 head = tail.m_free_list.m_next_free;
            tail.m_free_list.m_next_free = stable_vector::at(head).m_free_list.m_next_free;
            if (head == m_free_tail) {
                m_free_tail = none_v;
            }
            return head;
        }

        // start/end free blocks are push to the front of the free list,
        // middle blocks, which can't reallocated, are pushed to the back,
        // when start/end blocks are consumed, middle blocks are promoted to start/end blocks.
        constexpr void freePush_(const u32 index, sparse_vector_item &item, const bool push_tail = false) noexcept {
            if (m_free_tail != none_v) {
                sparse_vector_item &tail = stable_vector::at(m_free_tail);
                item.m_free_list.m_next_free = tail.m_free_list.m_next_free;
                tail.m_free_list.m_next_free = index;
                if (push_tail) {
                    m_free_tail = index;
                }
            } else {
                item.m_free_list.m_next_free = index;
                m_free_tail = index;
            }
        }

        [[nodiscard]] constexpr bool isFreeMiddleBlock_(const stable_vector_iterator &it) const noexcept {
            PPR_ASSERT(it->m_payload.m_skip && it->m_payload.m_seed == 0u);
            if (const u32 item_index = it.getIndex(); item_index == 0u || item_index + 1u == stable_vector::m_size) {
                return false;
            }

            stable_vector_iterator prev{it};
            --prev;
            if (prev->m_payload.m_skip == 0u) {
                PPR_ASSERT(prev->m_payload.m_seed);
                return false;
            }
            PPR_ASSERT(prev->m_payload.m_skip && prev->m_payload.m_seed == 0u);

            stable_vector_iterator next{it};
            ++next;
            if (next->m_payload.m_skip == 0u) {
                PPR_ASSERT(next->m_payload.m_seed);
                return false;
            }
            PPR_ASSERT(next->m_payload.m_skip && next->m_payload.m_seed == 0u);

            return true;
        }

        struct AllocationResult_ { // NOLINT(*-pro-type-member-init)
            sparse_vector_item *m_item_ptr;
            u32 m_index;

            [[nodiscard]] PPR_FORCE_INLINE SparseKeyId getKeyId() const noexcept {
                PPR_ASSERT(m_item_ptr != nullptr);
                return SparseKeyId{m_item_ptr->m_payload, m_index};
            }

            [[nodiscard]] PPR_FORCE_INLINE T *launderValuePtr() const noexcept {
                PPR_ASSERT(m_item_ptr != nullptr);
                return std::launder(m_item_ptr->getValuePtr());
            }
        };

        [[nodiscard]] AllocationResult_ allocateItemFromFreeList_() noexcept {
            for (;;) {
                const u32 new_alloc_index = freePopAssumeNotEmpty_();
                stable_vector_iterator new_alloc = stable_vector::begin() + new_alloc_index;
                PPR_ASSERT(new_alloc->m_payload.m_skip);

                // https://plflib.org/matt_bentley_-_the_low_complexity_jump-counting_pattern.pdf
                const u32 skip_left = (new_alloc_index > 0u ? (new_alloc - 1u)->m_payload.m_skip : 0u);
                const u32 skip_right = (new_alloc_index + 1u < stable_vector::m_size ? (new_alloc + 1u)->m_payload.m_skip : 0u);

                if (skip_left && skip_right) {
                    // middle-blocks can't be unskipped in O(1), pop until we have a front/back/single block
                    PPR_ASSERT(m_free_tail != none_v && "a middle block can't be alone in the free-list!");
                    freePush_(new_alloc_index, *new_alloc, true);
                    continue;
                }

                const u32 skip_len = new_alloc->m_payload.m_skip - 1u;
                if (skip_right) {
                    (new_alloc + 1u)->m_payload.m_skip = skip_len;
                    (new_alloc + skip_len)->m_payload.m_skip = skip_len;
                } else if (skip_left) {
                    (new_alloc - 1u)->m_payload.m_skip = skip_len;
                    (new_alloc - skip_len)->m_payload.m_skip = skip_len;
                }

                PPR_ASSERT(!isFreeMiddleBlock_(new_alloc));
                return AllocationResult_{.m_item_ptr = new_alloc.getPointer(), .m_index = new_alloc_index};
            }
        }

        [[nodiscard]] AllocationResult_ allocateItem_() noexcept(noexcept(stable_vector::pushBackUninitialized())) {
            AllocationResult_ new_alloc;
            if (stable_vector::m_size < stable_vector::m_capacity || m_free_tail == none_v) [[likely]] {
                sparse_vector_item *const free_ptr = stable_vector::pushBackUninitialized();
                PPR_ASSERT(stable_vector::m_capacity <= (1u << 24u) && "SparseVector do not support more than 16m elements");
                new_alloc = AllocationResult_{.m_item_ptr = free_ptr, .m_index = stable_vector::m_size - 1u};
            } else {
                new_alloc = allocateItemFromFreeList_();
            }

            PPR_ASSERT(new_alloc.m_item_ptr && new_alloc.m_index < stable_vector::m_size);
            mem::poisonIfDebug(mem::Poison::uninitialized, new_alloc.m_item_ptr->getValuePtr());

            new_alloc.m_item_ptr->m_payload.m_seed = nextAllocationSeed_();
            new_alloc.m_item_ptr->m_payload.m_skip = 0u;

            m_size_alive++;
            PPR_ASSERT(m_size_alive <= stable_vector::m_size);
            return new_alloc;
        }

        void deallocateItem_(const u32 old_alloc_index, sparse_vector_item &item) noexcept {
            PPR_ASSERT(old_alloc_index < stable_vector::m_capacity);
            PPR_ASSERT(item.m_payload.m_skip == 0u);
            PPR_ASSERT(m_size_alive > 0u);

            m_size_alive--;

            mem::poisonIfDebug(mem::Poison::destroyed, item.getValuePtr());

            new(std::launder(&item.m_free_list)) sparse_vector_item::FreeList{
                .m_next_free = none_v,
            };

            item.m_payload.m_seed = 0u;
            item.m_payload.m_skip = 1u;

            // https://plflib.org/matt_bentley_-_the_low_complexity_jump-counting_pattern.pdf
            const u32 skip_left = (old_alloc_index > 0u ? stable_vector::at(old_alloc_index - 1u).m_payload.m_skip : 0u);
            const u32 skip_right = (old_alloc_index + 1u < stable_vector::m_size ? stable_vector::at(old_alloc_index + 1u).m_payload.m_skip : 0u);

            bool is_middle_block = false;
            if (skip_left && skip_right) {
                is_middle_block = true;

                const u32 prev_node = old_alloc_index - skip_left;
                const u32 next_node = skip_right + old_alloc_index;
                const u32 skip_len = skip_left + skip_right + 1u;

                stable_vector::at(prev_node).m_payload.m_skip = skip_len;
                stable_vector::at(next_node).m_payload.m_skip = skip_len;
            } else if (skip_left) {
                const u32 prev_node = old_alloc_index - skip_left;
                const u32 skip_len = skip_left + 1u;

                stable_vector::at(prev_node).m_payload.m_skip = skip_len;
                item.m_payload.m_skip = skip_len;
            } else if (skip_right) {
                const u32 next_node = skip_right + old_alloc_index;
                const u32 skip_len = skip_right + 1u;

                stable_vector::at(next_node).m_payload.m_skip = skip_len;
                item.m_payload.m_skip = skip_len;
            }

            freePush_(old_alloc_index, item, is_middle_block);
        }

        using stable_vector::at;

    public:
        using value_type = T;
        using pointer = std::add_pointer_t<T>;
        using reference = std::add_lvalue_reference_t<T>;
        using size_type = std::size_t;

        using stable_vector::capacity;

        constexpr SparseVector() noexcept
            requires std::is_default_constructible_v<allocator_type> = default;

        explicit constexpr SparseVector(const std::size_t initial_capacity) noexcept
            requires std::is_default_constructible_v<allocator_type> {
            reserveAssumeEmpty(initial_capacity);
        }

        constexpr SparseVector(std::initializer_list<T> init_values) noexcept
            requires std::is_default_constructible_v<allocator_type> {
            assignAssumeEmpty(init_values);
        }

        explicit constexpr SparseVector(AllocatorT &&al) noexcept
            : stable_vector(std::move(al)) {
        }

        explicit constexpr SparseVector(const AllocatorT &al) noexcept
            : stable_vector(al) {
        }

        constexpr SparseVector(const std::size_t initial_capacity, AllocatorT &&al) noexcept
            : stable_vector(std::move(al)) {
            reserveAssumeEmpty(initial_capacity);
        }

        constexpr SparseVector(const std::size_t initial_capacity, const AllocatorT &al) noexcept
            : stable_vector(al) {
            reserveAssumeEmpty(initial_capacity);
        }

        constexpr SparseVector(std::initializer_list<T> init_values, AllocatorT &&al) noexcept
            : stable_vector(std::move(al)) {
            assignAssumeEmpty(init_values);
        }

        constexpr SparseVector(std::initializer_list<T> init_values, const AllocatorT &al) noexcept
            : stable_vector(al) {
            assignAssumeEmpty(init_values);
        }

        constexpr SparseVector(const SparseVector &other) noexcept
            : stable_vector(other.getAllocator_()) {
            assignAssumeEmpty(other);
        }

        constexpr SparseVector &operator =(const SparseVector &other) noexcept {
            if (this == &other) [[unlikely]] {
                return *this;
            }

            stable_vector::m_allocator = other.m_allocator;
            assign(other.view());
            return (*this);
        }

        constexpr SparseVector(SparseVector &&rvalue) noexcept
            : stable_vector(std::move(rvalue.getAllocator_())) {
            spliceAssumeEmpty(rvalue);
        }

        constexpr SparseVector &operator =(SparseVector &&rvalue) noexcept {
            if (this == &rvalue) [[unlikely]] {
                return *this;
            }

            reset();
            allocator_type::operator=(std::move(rvalue.getAllocator_()));
            spliceAssumeEmpty(rvalue);
            return *this;
        }

        constexpr ~SparseVector() noexcept {
            reset();
        }

        [[nodiscard]] constexpr bool isEmpty() const noexcept {
            return m_size_alive == 0u;
        }

        [[nodiscard]] constexpr std::size_t size() const noexcept {
            return m_size_alive;
        }

        using iterator = details::SparseVectorIterator<T, AllocatorT>;
        using const_iterator = details::SparseVectorIterator<const T, AllocatorT>;

        static_assert(std::bidirectional_iterator<iterator>);
        static_assert(std::bidirectional_iterator<const_iterator>);

        [[nodiscard]] constexpr iterator begin() noexcept {
            if (const auto first = stable_vector::begin(); first != stable_vector::end()) {
                return iterator(*this, first->m_payload.m_skip);
            }
            return end();
        }

        [[nodiscard]] constexpr iterator end() noexcept { return iterator(*this, umax_v); }

        [[nodiscard]] constexpr const_iterator begin() const noexcept {
            if (const auto first = stable_vector::begin(); first != stable_vector::end()) {
                return const_iterator(*this, first->m_payload.m_skip);
            }
            return end();
        }

        [[nodiscard]] constexpr const_iterator end() const noexcept { return const_iterator(*this, umax_v); }

        [[nodiscard]] constexpr const_iterator cbegin() const noexcept { return begin(); }
        [[nodiscard]] constexpr const_iterator cend() const noexcept { return end(); }

        [[nodiscard]] constexpr auto each(this auto &&self) noexcept {
            return std::ranges::subrange(self.begin(), self.end());
        }

        [[nodiscard]] static constexpr SparseKeyId key(const const_iterator &it) noexcept {
            if (PPR_ENSURE(it->m_payload.m_seed && it->m_payload.m_skip == 0u)) [[likely]] {
                return SparseKeyId(it->m_payload, it.getIndex());
            }
            return default_value_v;
        }

        [[nodiscard]] constexpr SparseKeyId key(const std::size_t index) noexcept {
            if (const sparse_vector_item &item = stable_vector::at(index);
                PPR_ENSURE(item.m_payload.m_seed && item.m_payload.m_skip == 0u)) [[likely]] {
                return SparseKeyId(item.m_payload, index);
            }
            return default_value_v;
        }

        [[nodiscard]] constexpr auto *tryGet(this auto &&self, const SparseKeyId key) noexcept {
            if (auto &item = self.at(key.m_index);
                item.m_payload.m_seed == key.m_seed) [[likely]] {
                PPR_ASSERT(item.m_payload.m_skip == 0u && item.m_payload.m_seed);
                return item.getValuePtr();
            } else {
                return static_cast<decltype(item.getValuePtr())>(nullptr);
            }
        }

        [[nodiscard]] constexpr auto &get(this auto &&self, const SparseKeyId key) noexcept {
            auto &item = self.at(key.m_index);
            PPR_ASSERT(item.m_payload.m_skip == 0u && item.m_payload.m_seed);
            PPR_ASSERT(item.m_payload.m_seed == key.m_seed);
            return *item.getValuePtr();
        }

        [[nodiscard]] PPR_FORCE_INLINE constexpr auto &
        operator [](this auto &&self, const SparseKeyId key) noexcept {
            return self.get(key);
        }

        [[nodiscard]] PPR_FORCE_INLINE constexpr bool contains(const SparseKeyId &key) const noexcept {
            return tryGet(key) != nullptr;
        }

        template<typename ValueT>
        [[nodiscard]] constexpr auto
        find(this auto &&self, ValueT &&value_to_find) noexcept
            requires std::equality_comparable_with<T, ValueT> {
            return std::find(self.begin(), self.end(), std::forward<ValueT>(value_to_find));
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
            for (auto &value: values) {
                add(value);
            }
        }

        [[nodiscard]] constexpr std::pair<SparseKeyId, T *>
        ddUninitialized() noexcept(std::is_nothrow_move_constructible_v<T>) {
            const AllocationResult_ alloc = allocateItem_();
            return std::make_pair(alloc.getKeyId(), alloc.launderValuePtr());
        }

        constexpr std::pair<SparseKeyId, T *>
        addDefault() noexcept(std::is_nothrow_move_constructible_v<T>)
            requires std::is_default_constructible_v<T> {
            const AllocationResult_ alloc = allocateItem_();
            T *const value_ptr = alloc.launderValuePtr();
            std::construct_at(value_ptr);
            return std::make_pair(alloc.getKeyId(), value_ptr);
        }

        constexpr SparseKeyId add(T &&rvalue) noexcept(std::is_nothrow_move_constructible_v<T>) {
            const AllocationResult_ alloc = allocateItem_();
            std::construct_at(alloc.launderValuePtr(), std::move(rvalue));
            return alloc.getKeyId();
        }

        constexpr SparseKeyId add(const T &value) noexcept(std::is_nothrow_copy_constructible_v<T>) {
            const AllocationResult_ alloc = allocateItem_();
            std::construct_at(alloc.launderValuePtr(), value);
            return alloc.getKeyId();
        }

        template<typename... ArgsT>
        constexpr iterator emplace(ArgsT &&... args)
            noexcept(std::is_nothrow_constructible_v<T, ArgsT &&...>)
            requires std::is_constructible_v<T, ArgsT &&...> {
            const AllocationResult_ alloc = allocateItem_();
            std::construct_at(alloc.launderValuePtr(), std::forward<ArgsT>(args)...);
            return iterator(*this, alloc.m_index);
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
                const std::size_t required = std::distance(first, last);
                reserveAdditional(required);
            }
            return appendAssumeCapacity(first, last);
        }

        template<std::forward_iterator IteratorT> requires details::is_iterator_of<IteratorT, T>
        [[maybe_unused]] constexpr std::size_t
        appendAssumeCapacity(IteratorT first, IteratorT last) noexcept(std::is_nothrow_copy_constructible_v<T>) {
            std::size_t n = 0u;
            for (; first != last; ++first, ++n) {
                add(*first);
            }
            return n;
        }

        constexpr bool erase(const SparseKeyId key) noexcept(std::is_nothrow_destructible_v<T>) {
            if (sparse_vector_item &item = stable_vector::at(key.m_index);
                item.m_payload.m_seed == key.m_seed) [[likely]] {
                PPR_ASSERT(item.m_payload.m_skip == 0u && item.m_payload.m_seed);

                std::destroy_at(item.getValuePtr());
                deallocateItem_(key.m_index, item);
                return true;
            }
            return false;
        }

        constexpr iterator erase(const const_iterator &it) noexcept(std::is_nothrow_destructible_v<T>) {
            PPR_ASSERT(it != end());
            const u32 item_index = it.getIndex();
            sparse_vector_item &item = stable_vector::at(item_index);
            PPR_ASSERT(item.m_payload.m_skip == 0u && item.m_payload.m_seed);

            std::destroy_at(item.getValuePtr());
            deallocateItem_(item_index, item);

            if (item_index + 1u < stable_vector::m_size) [[likely]] {
                stable_vector_iterator next(*this, item_index + 1u);
                return iterator(*this, next.getIndex() + next->m_payload.m_skip);
            }
            return end();
        }

        constexpr void reserveAssumeEmpty(const std::size_t n) noexcept {
            PPR_ASSERT(m_size_alive == 0u);
            PPR_ASSERT(m_free_tail == none_v);

            stable_vector::reserveAssumeEmpty(n);
            PPR_ASSERT(stable_vector::m_capacity <= (1u << 24u) && "SparseVector do not support more than 16m elements");
        }

        constexpr void reserveAdditional(const std::size_t n) noexcept {
            if (const u32 num_free_items = stable_vector::m_capacity - m_size_alive; n > num_free_items) [[unlikely]] {
                stable_vector::reserve(m_size_alive + n);
                PPR_ASSERT(stable_vector::m_capacity <= (1u << 24u) && "SparseVector do not support more than 16m elements");
            }
        }

        constexpr void reserve(const std::size_t n) noexcept {
            const u32 free_list_size = stable_vector::m_size - m_size_alive;
            stable_vector::reserve(n - free_list_size);
            PPR_ASSERT(stable_vector::m_capacity <= (1u << 24u) && "SparseVector do not support more than 16m elements");
        }

        constexpr void clear() noexcept(std::is_nothrow_destructible_v<T>) {
            if constexpr (!std::is_trivially_destructible_v<T>) {
                std::ranges::destroy(each());
            }

            stable_vector::m_size = 0u;
            m_free_tail = none_v;
            m_size_alive = 0u;
        }

        constexpr void reset() noexcept {
            clear(); // calls destructors
            stable_vector::reset(); // deallocate memory
        }

        constexpr void spliceAssumeEmpty(SparseVector &src) noexcept {
            PPR_ASSERT(m_size_alive == 0u);
            stable_vector::spliceAssumeEmpty(src);

            m_free_tail = src.m_free_tail;
            m_size_alive = src.m_size_alive;
            m_seed_alloc = src.m_seed_alloc;

            src.m_free_tail = none_v;
            src.m_size_alive = 0u;
            src.m_seed_alloc = 0u;
        }

        friend void swap(SparseVector &lhs, SparseVector &rhs) noexcept {
            using namespace std;
            swap(static_cast<stable_vector &>(lhs), static_cast<stable_vector &>(rhs));

            swap(lhs.m_free_tail, rhs.m_free_tail);

            // swap bit-fields manually
            {
                const u32 tmp = lhs.m_size_alive;
                lhs.m_size_alive = rhs.m_size_alive;
                rhs.m_size_alive = tmp;
            }
            {
                const u32 tmp = lhs.m_seed_alloc;
                lhs.m_seed_alloc = rhs.m_seed_alloc;
                rhs.m_seed_alloc = tmp;
            }
        }
    };

    // ------------------------------------------------------------------
    // CTAD
    // ------------------------------------------------------------------

    template<typename T>
    SparseVector(std::initializer_list<T>) -> SparseVector<std::remove_cvref_t<T> >;

    template<typename T, mem::details::TAllocator AllocatorT>
    SparseVector(std::initializer_list<T>, AllocatorT &&al) -> SparseVector<std::remove_cvref_t<T>, std::remove_cvref_t<AllocatorT> >;

    template<typename T, mem::details::TAllocator AllocatorT>
    SparseVector(std::initializer_list<T>, const AllocatorT &al) -> SparseVector<std::remove_cvref_t<T>, std::remove_cvref_t<AllocatorT> >;

    template<typename T, mem::details::TAllocator AllocatorT>
    struct details::relocatable<SparseVector<T, AllocatorT> > : relocatable<StableVector<T, AllocatorT> > {
    };
}
