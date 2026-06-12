module;
#include "pP/Macros.h"

export module engine.core:allocator;

import :assert;
import :containers;
import :memory_poison;

import std;

export namespace pP::mem {
    // ------------------------------------------------------------------
    // pointer memory aliasing
    // ------------------------------------------------------------------

    // test if a pointer overlaps the given memory range
    [[nodiscard]] constexpr bool overlap(const void *const storage, const std::size_t bytes, const void *const ptr) noexcept {
        return (static_cast<const std::byte *>(ptr) >= static_cast<const std::byte *>(storage) &&
                static_cast<const std::byte *>(ptr) < static_cast<const std::byte *>(storage) + bytes);
    }

    // test if a memory range overlaps another given memory range
    [[nodiscard]] constexpr bool overlap(const void *const storage, const std::size_t storage_size, const void *const ptr, const std::size_t ptr_size) noexcept {
        PPR_ASSERT(!overlap(storage, storage_size, ptr) || static_cast<const std::byte *>(ptr) + ptr_size <= static_cast<const std::byte *>(storage) + storage_size);
        return (static_cast<const std::byte *>(ptr) >= static_cast<const std::byte *>(storage) &&
                static_cast<const std::byte *>(ptr) + ptr_size <= static_cast<const std::byte *>(storage) + storage_size);
    }

    // ------------------------------------------------------------------
    // allocator concepts
    // ------------------------------------------------------------------

    namespace details {
        template<typename T>
        concept TAllocator = requires(std::remove_cvref_t<T> &al, const std::size_t bytes, const std::align_val_t alignment, void *const ptr)
        {
            { al.allocateRaw(bytes, alignment) } -> std::convertible_to<std::allocation_result<void *> >;
            { al.deallocateRaw(ptr, bytes, alignment) };
        };

        template<typename T>
        concept TOwningAllocator = requires(const std::remove_cvref_t<T> &al, const void *const ptr, const std::size_t size)
        {
            { al.owns(ptr, size) } -> std::convertible_to<bool>;
        } && TAllocator<T>;

        template<typename T>
        concept TResizableAllocator = requires(std::remove_cvref_t<T> &al, void *const ptr, const std::size_t old_size, std::size_t &new_size)
        {
            { al.resizeRaw(ptr, old_size, new_size) } -> std::convertible_to<bool>;
        } && TAllocator<T>;

        template<typename T>
        concept TBlockAllocator = requires()
        {
            { T::block_size_v } -> std::convertible_to<std::size_t>;
        } && TAllocator<T>;

        template<typename T>
        concept TArenaAllocator = requires(std::remove_cvref_t<T> &al, const void *const mark)
        {
            { al.watermark() } -> std::convertible_to<const void *>;
            { al.restore(mark) };
            { al.reset() };
        } && TOwningAllocator<T> && TResizableAllocator<T>;
    }

    // ------------------------------------------------------------------
    // allocation holds an allocation result managed by an external allocator
    // ------------------------------------------------------------------

    template<typename T, details::TAllocator AllocatorT,
        std::align_val_t AlignmentV = alignof_v<T>,
        std::unsigned_integral Size_T = std::size_t>
    class Allocation {
    public:
        static_assert(alignof(T) <= static_cast<std::size_t>(AlignmentV));
        static_assert(std::is_same_v<std::remove_cvref_t<T>, T>);

        std::allocation_result<void *, Size_T> m_block{};

        static constexpr bool is_stateless_v = std::is_empty_v<AllocatorT>;

        constexpr Allocation() noexcept = default;

        constexpr Allocation(Allocation &&rvalue) noexcept
            : m_block(std::move(rvalue.m_block)) {
            rvalue.m_block = {};
        }

        // can't implement since the allocator is not accessible here
        Allocation &operator =(Allocation &&rvalue) noexcept
            requires is_stateless_v {
            if (m_block.ptr != nullptr) {
                deallocate();
            }

            m_block = std::move(rvalue.m_block);
            rvalue.m_block = {};
            return *this;
        }

        Allocation(const Allocation &) = delete;

        Allocation &operator =(const Allocation &) = delete;

        constexpr Allocation(const std::size_t n, AllocatorT &al) noexcept {
            allocate(al, n);
        }

        constexpr Allocation(std::type_identity_t<T>, const std::size_t n, AllocatorT &al) noexcept
            : Allocation(n, al) {
        }

        explicit constexpr Allocation(const std::size_t n) noexcept
            requires is_stateless_v {
            allocate(n);
        }

        constexpr ~Allocation() noexcept {
            if constexpr (is_stateless_v) {
                deallocate();
            }
            PPR_ASSERT(m_block.ptr == nullptr && "Allocation was not deallocated before destruction");
        }

        [[nodiscard]] PPR_FORCE_INLINE constexpr bool isValid() const noexcept {
            return m_block.ptr != nullptr;
        }

        [[nodiscard]] PPR_FORCE_INLINE constexpr T *data() const noexcept {
            return static_cast<T *>(m_block.ptr);
        }

        [[nodiscard]] PPR_FORCE_INLINE constexpr std::size_t count() const noexcept {
            return m_block.count / sizeof(T);
        }

        [[nodiscard]] PPR_FORCE_INLINE constexpr std::size_t size_bytes() const noexcept {
            return m_block.count;
        }

        [[nodiscard]] PPR_FORCE_INLINE static constexpr std::align_val_t alignment() noexcept {
            return AlignmentV;
        }

        [[nodiscard]] PPR_FORCE_INLINE constexpr T &operator*() const noexcept {
            return *static_cast<T *>(m_block.ptr);
        }

        [[nodiscard]] PPR_FORCE_INLINE constexpr T *operator->() const noexcept {
            return static_cast<T *>(m_block.ptr);
        }

        [[nodiscard]] PPR_FORCE_INLINE constexpr bool owns(const void *const ptr, const std::size_t size) const noexcept {
            return overlap(data(), size_bytes(), ptr, size);
        }

        [[nodiscard]] PPR_FORCE_INLINE constexpr std::span<T> view() const noexcept {
            PPR_ASSERT(isValid() && "Allocation is not valid");
            return std::span<T>(data(), count());
        }

        /// forget the allocation: it won't be deallocated by the destructor
        [[nodiscard]] PPR_FORCE_INLINE constexpr std::span<T> discard() noexcept {
            std::span<T> discarded{};
            if (T *const p_data = data()) [[likely]] {
                discarded = std::span<T>{p_data, count()};
                m_block = {}; // reset to default value
            }
            return discarded;
        }

        [[maybe_unused]] constexpr std::allocation_result<T *>
        allocate(AllocatorT &al, const std::size_t n = 1u) noexcept {
            PPR_ASSERT(!isValid() && "Allocation is already valid");
            const std::allocation_result<void *> blk = al.allocateRaw(sizeof(T) * n, AlignmentV);
            m_block.ptr = blk.ptr;
            m_block.count = checked_cast<Size_T>(blk.count);
            return {static_cast<T *>(m_block.ptr), m_block.count / sizeof(T)};
        }

        [[maybe_unused]] constexpr std::allocation_result<T *>
        allocate(const std::size_t n = 1u) noexcept
            requires is_stateless_v {
            AllocatorT al{};
            return allocate(al, n);
        }

        [[nodiscard]] constexpr bool
        resize(AllocatorT &al, const std::size_t new_size) noexcept
            requires details::TResizableAllocator<AllocatorT> {
            if (isValid()) [[likely]] {
                if (const std::size_t raw_size = new_size * sizeof(T);
                    al.resizeRaw(m_block.ptr, m_block.count, raw_size)) {
                    m_block.count = checked_cast<Size_T>(raw_size);
                    return true;
                }
                return false;
            }
            if (new_size > 0u) [[likely]] {
                allocate(al, new_size);
                return isValid();
            }
            return false;
        }

        [[nodiscard]] constexpr bool resize(const std::size_t new_size) noexcept
            requires is_stateless_v {
            AllocatorT al{};
            return resize(al, new_size);
        }

        [[maybe_unused]] constexpr std::allocation_result<T *>
        relocate(AllocatorT &al, const std::size_t new_size) noexcept
            requires pP::details::is_relocatable_v<T> {
            if (m_block.ptr) [[likely]] {
                if (new_size == 0u) [[unlikely]] {
                    deallocate(al);
                    return {};
                }

                bool skip_relocate = false;
                if constexpr (details::TResizableAllocator<AllocatorT>) {
                    skip_relocate = resize(al, new_size);
                }

                if (!skip_relocate) [[likely]] {
                    const std::size_t raw_size = new_size * sizeof(T);
                    std::allocation_result<void *> new_block = al.allocateRaw(raw_size, AlignmentV);

                    std::memcpy(new_block.ptr, m_block.ptr, std::min(raw_size, m_block.count));
                    al.deallocateRaw(m_block.ptr, m_block.count, AlignmentV);

                    m_block = std::move(new_block);
                }

                return {static_cast<T *>(m_block.ptr), m_block.count / sizeof(T)};
            }
            return allocate(new_size);
        }

        [[maybe_unused]] constexpr std::allocation_result<T *>
        relocate(const std::size_t new_size) noexcept
            requires is_stateless_v && pP::details::is_relocatable_v<T> {
            AllocatorT al{};
            return relocate(al, new_size);
        }

        constexpr void deallocate(AllocatorT &al) noexcept {
            if (isValid()) [[likely]] {
                al.deallocateRaw(m_block.ptr, m_block.count, AlignmentV);
                m_block = {};
            }
        }

        constexpr void deallocate() noexcept requires is_stateless_v {
            AllocatorT al{};
            deallocate(al);
        }

        constexpr void deallocateAssumeNotEmpty(AllocatorT &al) noexcept {
            al.deallocateRaw(m_block.ptr, m_block.count, AlignmentV);
            m_block = {};
        }

        constexpr void deallocateAssumeNotEmpty() noexcept requires is_stateless_v {
            AllocatorT al{};
            deallocateAssumeNotEmpty(al);
        }

        template<typename... ArgsT> requires std::is_constructible_v<T, ArgsT &&...>
        [[nodiscard]] constexpr T *create(AllocatorT &al, ArgsT &&... args)
            noexcept(std::is_nothrow_constructible_v<T, ArgsT &&...>) {
            allocate(al, 1u);
            PPR_ASSERT(m_block.ptr);
            return std::construct_at(static_cast<T *>(m_block.ptr), std::forward<ArgsT>(args)...);
        }

        template<typename... ArgsT> requires std::is_constructible_v<T, ArgsT &&...>
        [[nodiscard]] constexpr T *create(ArgsT &&... args)
            noexcept(std::is_nothrow_constructible_v<T, ArgsT &&...>)
            requires is_stateless_v {
            AllocatorT al{};
            return create(al, std::forward<ArgsT>(args)...);
        }

        constexpr void destroy(AllocatorT &al) noexcept(std::is_nothrow_destructible_v<T>) {
            if (isValid()) [[likely]] {
                std::destroy_at(static_cast<T *>(m_block.ptr));
                al.deallocateRaw(m_block.ptr, m_block.count, AlignmentV);
                m_block = {};
            }
        }

        constexpr void destroy() noexcept(std::is_nothrow_destructible_v<T>)
            requires is_stateless_v {
            AllocatorT al{};
            destroy(al);
        }

        [[nodiscard]] constexpr bool operator ==(const Allocation &other) const noexcept = default;

        [[nodiscard]] constexpr bool operator !=(const Allocation &other) const noexcept = default;
    };

    // ------------------------------------------------------------------
    // allocator traits
    // ------------------------------------------------------------------

    template<typename AllocatorT>
    struct AllocatorTraits {
        // ------------------------------------------------------------------
        // trivial types
        // ------------------------------------------------------------------

        template<typename T>
        [[nodiscard]] constexpr T *
        allocate(this AllocatorT &al, const std::size_t n = 1u,
                 const std::align_val_t alignment = alignof_v<T>) noexcept
            requires details::TAllocator<AllocatorT> {
            return std::launder(static_cast<T *>(al.allocateRaw(sizeof(T) * n, alignment).ptr));
        }

        template<typename T>
        [[nodiscard]] constexpr auto
        allocate_at_least(this AllocatorT &al, const std::size_t n = 1u,
                          const std::align_val_t alignment = alignof_v<T>) noexcept -> std::allocation_result<T *> {
            const std::allocation_result<void *> raw = al.allocateRaw(sizeof(T) * n, alignment);
            return std::allocation_result<T *>(static_cast<T *>(raw.ptr), raw.count / sizeof(T));
        }

        template<typename T>
        constexpr void deallocate(this AllocatorT &al, T *const ptr, const std::size_t n = 1u,
                                  const std::align_val_t alignment = alignof_v<T>) noexcept
            requires details::TAllocator<AllocatorT> {
            al.deallocateRaw(ptr, sizeof(T) * n, alignment);
        }

        template<typename T>
        [[nodiscard]] constexpr bool resize(this AllocatorT &al, T *const ptr, const std::size_t old_n, const std::size_t new_n) noexcept
            requires details::TResizableAllocator<AllocatorT> {
            return al.resizeRaw(ptr, sizeof(T) * old_n, sizeof(T) * new_n);
        }

        // ------------------------------------------------------------------
        // fast relocation, overload Relocatable<> to enable for your types
        // ------------------------------------------------------------------

        template<typename T> requires pP::details::is_relocatable_v<T>
        [[nodiscard]] constexpr std::allocation_result<T *>
        relocate(this AllocatorT &al, T *const ptr, const std::size_t old_n, const std::size_t new_n,
                 const std::align_val_t alignment = alignof_v<T>) noexcept
            requires pP::details::is_relocatable_v<T> {
            if (ptr) [[likely]] {
                if (new_n == 0u) [[unlikely]] {
                    al.deallocate(ptr, old_n, alignment);
                    return {};
                }

                bool skip_relocate = false;
                if constexpr (details::TResizableAllocator<AllocatorT>) {
                    skip_relocate = al.resize(ptr, old_n, new_n);
                }

                if (skip_relocate) [[likely]] {
                    return {ptr, new_n};
                }
            } else if (new_n == 0u) [[unlikely]] {
                return {};
            }

            T *const new_block = al.template allocate<T>(new_n, alignment);
            PPR_ASSERT(new_block);

            if (ptr) {
                std::memcpy(new_block, ptr, std::min(new_n, old_n) * sizeof(T));
                al.deallocate(ptr, old_n, alignment);
            }

            return {new_block, new_n};
        }

        // ------------------------------------------------------------------
        // Object construction helpers
        // ------------------------------------------------------------------

        template<typename T, typename... ArgsT>
            requires std::is_constructible_v<T, ArgsT &&...>
        [[nodiscard]] constexpr T *
        create(this AllocatorT &al, ArgsT&&... args)
            noexcept(std::is_nothrow_constructible_v<T, ArgsT &&...>) {
            if (T *const ptr = al.template allocate<T>()) [[likely]] {
                std::construct_at(ptr, std::forward<ArgsT>(args)...);
                return ptr;
            }
            return nullptr;
        }

        template<typename T>
            requires std::is_copy_constructible_v<T>
        [[nodiscard]] constexpr Allocation<T, AllocatorT>
        create(this AllocatorT &al, const T &copy)
            noexcept(std::is_nothrow_copy_constructible_v<T>) {
            return al.template construct<T>(copy);
        }

        template<typename T>
            requires std::is_move_constructible_v<T>
        [[nodiscard]] constexpr Allocation<T, AllocatorT>
        create(this AllocatorT &al, T &&move)
            noexcept(std::is_nothrow_move_constructible_v<T>) {
            return al.template construct<T>(std::move(move));
        }

        template<typename T> requires std::is_destructible_v<T>
        constexpr void destroy(this AllocatorT &al, T *ptr)
            noexcept(std::is_nothrow_destructible_v<T>) {
            if (PPR_ENSURE(ptr != nullptr)) [[likely]] {
                std::destroy_at(ptr);
                al.deallocate(ptr);
            }
        }

        // ------------------------------------------------------------------
        // Relocatable range helpers
        // ------------------------------------------------------------------

        template<typename T>
            requires pP::details::is_relocatable_v<T>
        [[nodiscard]] constexpr std::span<T>
        span(this AllocatorT &al, const std::size_t n, const std::align_val_t alignment = alignof_v<T>) noexcept
            requires details::TAllocator<AllocatorT> {
            const std::allocation_result<T *> block = al.template allocate_at_least<T>(n, alignment);
            return std::span<T>(block.ptr, block.count);
        }

        template<std::ranges::contiguous_range RangeT>
            requires pP::details::is_relocatable_v<std::ranges::range_value_t<RangeT>>
        [[nodiscard]] constexpr std::span<std::ranges::range_value_t<RangeT>>
        dup(this AllocatorT &al, RangeT &&values, const std::align_val_t alignment = alignof_v<std::ranges::range_value_t<RangeT>>) noexcept
            requires details::TAllocator<AllocatorT> {
            using value_type = std::ranges::range_value_t<RangeT>;
            const std::size_t n = std::ranges::size(values);
            const std::span<value_type> dst{al.template allocate<value_type>(n, alignment), n};
            std::memcpy(dst.data(), std::ranges::data(values), n * sizeof(value_type));
            return dst;
        }
    };

    // ------------------------------------------------------------------
    // allocator wrapper for containers
    // ------------------------------------------------------------------

    template<details::TAllocator AllocatorT>
    class PPR_EMPTY_BASES Allocator;

    template<details::TAllocator AllocatorT>
    struct Inplace : AllocatorT {
        static_assert(std::is_same_v<AllocatorT, std::remove_cvref_t<AllocatorT> >);

        using allocator_type = AllocatorT;
        using allocator_type::allocator_type;

        using allocator_type::allocateRaw;
        using allocator_type::deallocateRaw;

        Inplace(const Inplace &) = delete;
        Inplace &operator=(const Inplace &) = delete;
    };

    namespace details {
        template<TAllocator AllocatorT>
        struct use_inplace : std::is_empty<AllocatorT> {
        };

        template<TAllocator AllocatorT>
        struct use_inplace<Inplace<AllocatorT> > : std::true_type {
        };

        template<TAllocator AllocatorT>
        inline constexpr bool use_inplace_v = use_inplace<AllocatorT>::value;

        template<TAllocator AllocatorT>
        struct unwrap_inplace : std::type_identity<AllocatorT> {
        };

        template<TAllocator AllocatorT>
        struct unwrap_inplace<Inplace<AllocatorT> > : std::type_identity<AllocatorT> {
        };

        template<TAllocator AllocatorT>
        using unwrap_inplace_t = unwrap_inplace<AllocatorT>::type;

        template<TAllocator AllocatorT>
        struct allocator_ref {
            using type = std::conditional_t<
                use_inplace_v<AllocatorT>,
                AllocatorT,
                std::reference_wrapper<AllocatorT> >;
        };

        template<TAllocator AllocatorT>
        struct allocator_ref<Allocator<AllocatorT> > {
            using type = allocator_ref<AllocatorT>::type;
        };

        template<TAllocator AllocatorT>
        using allocator_ref_t = allocator_ref<AllocatorT>::type;
    }

    template<details::TAllocator AllocatorT>
    using AllocatorForceRef = Allocator<details::unwrap_inplace_t<AllocatorT> >;

    // compliant with std::allocator requirements from C++23
    template<details::TAllocator AllocatorT>
    class PPR_EMPTY_BASES Allocator : details::allocator_ref_t<AllocatorT>,
                                      public AllocatorTraits<Allocator<AllocatorT> > {
        static_assert(std::is_same_v<AllocatorT, std::remove_cvref_t<AllocatorT> >);

    public:
        static constexpr bool is_stateless_v = std::is_empty_v<AllocatorT>;

        using reference = details::allocator_ref_t<AllocatorT>;

        constexpr Allocator()
            noexcept(std::is_nothrow_default_constructible_v<reference>)
            requires std::is_default_constructible_v<reference> = default;

        // ReSharper disable once CppNonExplicitConvertingConstructor
        constexpr Allocator(AllocatorT &value)
            noexcept(std::is_nothrow_constructible_v<reference, AllocatorT &>)
            requires std::is_constructible_v<reference, AllocatorT &>
            : reference(value) {
        }

        // ReSharper disable once CppNonExplicitConvertingConstructor
        constexpr Allocator(const AllocatorT &value)
            noexcept(std::is_nothrow_constructible_v<reference, const AllocatorT &>)
            requires std::is_constructible_v<reference, const AllocatorT &>
            : reference(value) {
        }

        // ReSharper disable once CppNonExplicitConvertingConstructor
        constexpr Allocator(AllocatorT &&value)
            noexcept(std::is_nothrow_constructible_v<reference, AllocatorT &&>)
            requires std::is_constructible_v<reference, AllocatorT &&>
            : reference(std::move(value)) {
        }

        template<typename... ArgsT>
        explicit constexpr Allocator([[maybe_unused]] std::in_place_t, ArgsT &&... args)
            noexcept(std::is_nothrow_constructible_v<reference, ArgsT &&...>)
            requires std::is_constructible_v<reference, ArgsT &&...>
            : reference(std::forward<ArgsT>(args)...) {
        }

        constexpr Allocator(const Allocator &) noexcept = default;

        constexpr Allocator &operator=(const Allocator &) noexcept = default;

        constexpr Allocator(Allocator &&) noexcept = default;

        constexpr Allocator &operator=(Allocator &&) noexcept = default;

        // ReSharper disable once CppNonExplicitConversionOperator
        [[nodiscard]] constexpr operator AllocatorT &() noexcept {
            return materialize();
        }

        // ReSharper disable once CppNonExplicitConversionOperator
        [[nodiscard]] constexpr operator const AllocatorT &() const noexcept {
            return materialize();
        }

        [[nodiscard]] PPR_FORCE_INLINE constexpr AllocatorT &materialize() noexcept {
            return static_cast<reference &>(*this);
        }

        [[nodiscard]] PPR_FORCE_INLINE constexpr const AllocatorT &materialize() const noexcept {
            return static_cast<const reference &>(*this);
        }

        [[nodiscard]] constexpr AllocatorForceRef<AllocatorT> forceRef() noexcept {
            return AllocatorForceRef<AllocatorT>(materialize());
        }

        [[nodiscard]] constexpr std::allocation_result<void *>
        allocateRaw(const std::size_t bytes, const std::align_val_t alignment = max_align_v) noexcept {
            return materialize().allocateRaw(bytes, alignment);
        }

        constexpr auto deallocateRaw(void *const ptr, const std::size_t bytes, const std::align_val_t alignment = max_align_v) noexcept {
            return materialize().deallocateRaw(ptr, bytes, alignment);
        }

        [[nodiscard]] constexpr bool owns(const void *const ptr, const std::size_t size) const noexcept
            requires details::TOwningAllocator<AllocatorT> {
            return materialize().owns(ptr, size);
        }

        [[nodiscard]] constexpr bool
        resizeRaw(void *const ptr, const std::size_t old_size, const std::size_t new_size) noexcept
            requires details::TResizableAllocator<AllocatorT> {
            return materialize().resizeRaw(ptr, old_size, new_size);
        }

        [[nodiscard]] constexpr const void *
        watermark() const noexcept
            requires details::TArenaAllocator<AllocatorT> {
            return materialize().watermark();
        }

        void restore(const void *const mark) noexcept
            requires details::TArenaAllocator<AllocatorT> {
            materialize().restore(mark);
        }

        void reset() noexcept
            requires details::TArenaAllocator<AllocatorT> {
            materialize().reset();
        }
    };

    template<details::TAllocator AllocatorT>
    Allocator(AllocatorT &al) -> Allocator<std::remove_cvref_t<AllocatorT> >;

    template<details::TAllocator AllocatorT>
    Allocator(const AllocatorT &al) -> Allocator<std::remove_cvref_t<AllocatorT> >;

    // ------------------------------------------------------------------
    // fallback to secondary if primary fails, and deallocate from the owning allocator
    // ------------------------------------------------------------------

    template<details::TOwningAllocator PrimaryT, details::TAllocator SecondaryT>
    class PPR_EMPTY_BASES Fallback : PrimaryT, SecondaryT {
    public:
        constexpr Fallback() requires std::is_default_constructible_v<PrimaryT> &&
                                      std::is_default_constructible_v<SecondaryT> = default;

        // ReSharper disable once CppNonExplicitConvertingConstructor
        constexpr Fallback(PrimaryT &&primary)
            noexcept(std::is_nothrow_move_constructible_v<PrimaryT> && std::is_nothrow_default_constructible_v<SecondaryT>)
            requires std::is_move_constructible_v<PrimaryT> && std::is_default_constructible_v<SecondaryT>
            : PrimaryT(std::move(primary)) {
        }

        // ReSharper disable once CppNonExplicitConvertingConstructor
        constexpr Fallback(SecondaryT &&secondary)
            noexcept(std::is_nothrow_move_constructible_v<SecondaryT> && std::is_nothrow_default_constructible_v<PrimaryT>)
            requires std::is_move_constructible_v<SecondaryT> && std::is_default_constructible_v<PrimaryT>
            : SecondaryT(std::move(secondary)) {
        }

        constexpr Fallback(PrimaryT &&primary, SecondaryT &&secondary)
            noexcept(std::is_nothrow_move_constructible_v<PrimaryT> && std::is_nothrow_move_constructible_v<SecondaryT>)
            requires std::is_move_constructible_v<PrimaryT> && std::is_move_constructible_v<SecondaryT>
            : PrimaryT(std::move(primary)),
              SecondaryT(std::move(secondary)) {
        }

        // ReSharper disable once CppNonExplicitConvertingConstructor
        constexpr Fallback(const PrimaryT &primary)
            noexcept(std::is_nothrow_copy_constructible_v<PrimaryT> && std::is_nothrow_default_constructible_v<SecondaryT>)
            requires std::is_copy_constructible_v<PrimaryT> && std::is_default_constructible_v<SecondaryT>
            : PrimaryT(primary) {
        }

        // ReSharper disable once CppNonExplicitConvertingConstructor
        constexpr Fallback(const SecondaryT &secondary)
            noexcept(std::is_nothrow_copy_constructible_v<SecondaryT> && std::is_nothrow_default_constructible_v<PrimaryT>)
            requires std::is_copy_constructible_v<SecondaryT> && std::is_default_constructible_v<PrimaryT>
            : SecondaryT(secondary) {
        }

        constexpr Fallback(const PrimaryT &primary, const SecondaryT &secondary)
            noexcept(std::is_nothrow_copy_constructible_v<PrimaryT> && std::is_nothrow_copy_constructible_v<SecondaryT>)
            requires std::is_copy_constructible_v<PrimaryT> && std::is_copy_constructible_v<SecondaryT>
            : PrimaryT(primary),
              SecondaryT(secondary) {
        }

        [[nodiscard]] constexpr std::allocation_result<void *>
        allocateRaw(const std::size_t bytes, const std::align_val_t alignment) noexcept {
            if (const std::allocation_result<void *> block = PrimaryT::allocateRaw(bytes, alignment); block.ptr) [[likely]] {
                return block;
            }
            return SecondaryT::allocateRaw(bytes, alignment);
        }

        constexpr void deallocateRaw(void *const ptr, const std::size_t bytes, const std::align_val_t alignment) noexcept {
            if (PrimaryT::owns(ptr, bytes)) [[likely]] {
                PrimaryT::deallocateRaw(ptr, bytes, alignment);
            } else {
                SecondaryT::deallocateRaw(ptr, bytes, alignment);
            }
        }

        [[nodiscard]] constexpr bool
        resizeRaw(void *const ptr, const std::size_t old_size, const std::size_t new_size) noexcept
            requires details::TResizableAllocator<PrimaryT> ||
                     details::TResizableAllocator<SecondaryT> {
            if (PrimaryT::owns(ptr, old_size)) [[likely]] {
                if constexpr (details::TResizableAllocator<PrimaryT>) {
                    return PrimaryT::resizeRaw(ptr, old_size, new_size);
                } else {
                    return false;
                }
            }
            if constexpr (details::TResizableAllocator<SecondaryT>) {
                return SecondaryT::resizeRaw(ptr, old_size, new_size);
            } else {
                return false;
            }
        }
    };

    // ------------------------------------------------------------------
    // Use UnderT if size is under or equal to SizeThreshold, AboveT else
    // ------------------------------------------------------------------

    template<details::TAllocator UnderT, std::size_t SizeThresholdV, details::TAllocator AboveT>
    class Threshold : UnderT, AboveT {
    public:
        static constexpr std::size_t size_threshold = SizeThresholdV;

        constexpr Threshold() requires std::is_default_constructible_v<UnderT> &&
                                       std::is_default_constructible_v<AboveT> = default;

        explicit constexpr Threshold(UnderT &&under)
            noexcept(std::is_nothrow_move_constructible_v<UnderT> && std::is_nothrow_default_constructible_v<AboveT>)
            requires std::is_move_constructible_v<UnderT> && std::is_default_constructible_v<AboveT>
            : UnderT(std::move(under)) {
        }

        explicit constexpr Threshold(AboveT &&above)
            noexcept(std::is_nothrow_move_constructible_v<AboveT> && std::is_nothrow_default_constructible_v<UnderT>)
            requires std::is_move_constructible_v<AboveT> && std::is_default_constructible_v<UnderT>
            : AboveT(std::move(above)) {
        }

        constexpr Threshold(UnderT &&under, AboveT &&above)
            noexcept(std::is_nothrow_move_constructible_v<UnderT> && std::is_nothrow_move_constructible_v<AboveT>)
            requires std::is_move_constructible_v<UnderT> && std::is_move_constructible_v<AboveT>
            : UnderT(std::move(under)),
              AboveT(std::move(above)) {
        }

        explicit constexpr Threshold(const UnderT &under)
            noexcept(std::is_nothrow_copy_constructible_v<UnderT> && std::is_nothrow_default_constructible_v<AboveT>)
            requires std::is_copy_constructible_v<UnderT> && std::is_default_constructible_v<AboveT>
            : UnderT(under) {
        }

        constexpr Threshold(const UnderT &under, const AboveT &above)
            noexcept(std::is_nothrow_copy_constructible_v<UnderT> && std::is_nothrow_copy_constructible_v<AboveT>)
            requires std::is_copy_constructible_v<UnderT> && std::is_copy_constructible_v<AboveT>
            : UnderT(under),
              AboveT(above) {
        }

        [[nodiscard]] constexpr std::allocation_result<void *>
        allocateRaw(const std::size_t bytes, const std::align_val_t alignment) noexcept {
            if (bytes <= size_threshold) {
                return UnderT::allocateRaw(bytes, alignment);
            }
            return AboveT::allocateRaw(bytes, alignment);
        }

        constexpr void deallocateRaw(void *const ptr, const std::size_t bytes, const std::align_val_t alignment) noexcept {
            if (bytes <= size_threshold) {
                UnderT::deallocateRaw(ptr, bytes, alignment);
            } else {
                AboveT::deallocateRaw(ptr, bytes, alignment);
            }
        }

        [[nodiscard]] constexpr bool
        resizeRaw(void *const ptr, const std::size_t old_size, const std::size_t new_size) noexcept
            requires details::TResizableAllocator<UnderT> ||
                     details::TResizableAllocator<AboveT> {
            if (old_size <= size_threshold) {
                if constexpr (details::TResizableAllocator<UnderT>) {
                    if (new_size <= size_threshold) {
                        return UnderT::resizeRaw(ptr, old_size, new_size);
                    }
                }
                return false;
            }
            if constexpr (details::TResizableAllocator<AboveT>) {
                if (new_size > size_threshold) {
                    return AboveT::resizeRaw(ptr, old_size, new_size);
                }
            }
            return false;
        }
    };

    // ------------------------------------------------------------------
    // allocate at most one block using all in-situ storage available
    // ------------------------------------------------------------------

    template<std::size_t InSituSizeV, std::align_val_t AlignmentV = max_align_v>
    class alignas(static_cast<std::size_t>(AlignmentV)) InSitu {
        enum EStatus_ : u8 {
            status_free_ = 0x7F,
            status_used_ = 0x7A,
        };

        alignas(static_cast<std::size_t>(AlignmentV)) std::byte m_storage[InSituSizeV];
        EStatus_ m_status{status_free_};

    public:
#if PPR_ENABLE_ASSERTIONS
        constexpr InSitu() noexcept { // NOLINT(*-pro-type-member-init)
            poisonReserved(&m_storage);
        }

        constexpr ~InSitu() noexcept {
            PPR_ASSERT(m_status == status_free_ && "In-situ buffer is still in use during destruction");
            m_status = status_used_; // disable use-after-free
        }
#endif

        InSitu(const InSitu &) = delete;
        InSitu &operator=(const InSitu &) = delete;

        [[nodiscard]] constexpr bool owns(const void *const ptr, const std::size_t size) const noexcept {
            const bool owned = overlap(m_storage, InSituSizeV, ptr, size);
            PPR_ASSERT((!owned || m_status == status_used_) && "Trying to access an exhausted in-situ buffer");
            return owned;
        }

        [[nodiscard]] constexpr std::allocation_result<void *>
        allocateRaw(const std::size_t bytes, const std::align_val_t alignment) noexcept {
            if (m_status == status_used_) [[unlikely]] {
                return {}; // OOM — let caller decide
            }

            void *aligned_ptr = m_storage;
            std::size_t space = sizeof(m_storage);
            if (std::align(static_cast<std::size_t>(alignment), bytes, aligned_ptr, space) == nullptr) [[unlikely]] {
                return {}; // OOM — let caller decide
            }

            m_status = status_used_;
            unpoisonUninitialized(aligned_ptr, space);
            return {aligned_ptr, space};
        }

        constexpr void deallocateRaw([[maybe_unused]] const void *const ptr,
                                     [[maybe_unused]] const std::size_t bytes,
                                     [[maybe_unused]] const std::align_val_t alignment) noexcept {
            PPR_ASSERT(m_status == status_used_ && "Trying to deallocate an in-situ buffer that isn't exhausted");
            PPR_ASSERT(overlap(m_storage, InSituSizeV, ptr, bytes) && "Trying to deallocate a pointer outside of the in-situ buffer");

            m_status = status_free_;
            poisonReserved(&m_storage);
        }

        [[nodiscard]] constexpr bool
        resizeRaw(void *const ptr, [[maybe_unused]] const std::size_t old_size, const std::size_t new_size) noexcept {
            PPR_ASSERT(ptr && m_status == status_used_);
            PPR_ASSERT(overlap(m_storage, InSituSizeV, ptr, old_size) && "Trying to resize a pointer outside of the in-situ buffer");
            PPR_ASSUME(ptr != nullptr);
            if (static_cast<const std::byte *>(ptr) + new_size <= static_cast<const std::byte *>(m_storage) + InSituSizeV) {
                if (old_size < new_size) {
                    unpoisonUninitialized(static_cast<std::byte *>(ptr) + old_size, new_size - old_size);
                }
                return true;
            }
            return false;
        }
    };

    template<std::size_t InSituSizeV,
        details::TAllocator FallbackT,
        std::align_val_t AlignmentV = max_align_v>
        requires (InSituSizeV > 0)
    using InSituFallback = Inplace<Fallback<InSitu<InSituSizeV, AlignmentV>, FallbackT> >;

    template<std::size_t InSituSizeV,
        details::TAllocator FallbackT,
        std::align_val_t AlignmentV = max_align_v>
        requires (InSituSizeV > 0)
    using InSituThreshold = Inplace<Threshold<InSitu<InSituSizeV, AlignmentV>, InSituSizeV, FallbackT> >;

    // ------------------------------------------------------------------
    // Most Recently Used block cache
    // ------------------------------------------------------------------

    template<std::size_t BlockSizeV, details::TAllocator AllocatorT, std::size_t MaxNumBlocks = 2u, std::align_val_t AlignmentV = max_align_v>
        requires (BlockSizeV > 0u && MaxNumBlocks > 0u)
    class LocalCache : AllocatorT {
        RingBuffer<void *, MaxNumBlocks> m_mru{};

    public:
        static constexpr std::size_t block_size_v = BlockSizeV;

        constexpr LocalCache() noexcept(std::is_nothrow_constructible_v<AllocatorT>) = default;

        explicit constexpr LocalCache(const AllocatorT &alloc)
            noexcept(std::is_nothrow_copy_constructible_v<AllocatorT>)
            : AllocatorT(alloc) {
        }

        explicit constexpr LocalCache(AllocatorT &&alloc)
            noexcept(std::is_nothrow_move_constructible_v<AllocatorT>)
            : AllocatorT(std::move(alloc)) {
        }

        constexpr ~LocalCache() noexcept {
            shrinkToFit();
        }

        constexpr void shrinkToFit() noexcept {
            for (void *const p_block: m_mru) {
                PPR_ASSERT(p_block != nullptr);
                AllocatorT::deallocateRaw(p_block, block_size_v, AlignmentV);
            }

            m_mru.clear();
        }

        [[nodiscard]] constexpr bool owns(const void *const ptr, const std::size_t size) const noexcept
            requires details::TOwningAllocator<AllocatorT> {
            return AllocatorT::owns(ptr, size);
        }

        [[nodiscard]] constexpr std::allocation_result<void *>
        allocateRaw(const std::size_t bytes, const std::align_val_t alignment) noexcept {
            PPR_ASSERT(bytes == block_size_v);
            PPR_ASSERT(alignment <= AlignmentV);

            if (m_mru.size() > 0u) [[likely]] {
                void *const p_block = m_mru.popBackAssumeNotEmpty();
                unpoisonUninitialized(p_block, block_size_v);
                return {p_block, block_size_v};
            }

            return AllocatorT::allocateRaw(bytes, AlignmentV);
        }

        constexpr void deallocateRaw(void *const ptr,
                                     [[maybe_unused]] const std::size_t bytes,
                                     [[maybe_unused]] const std::align_val_t alignment) noexcept {
            PPR_ASSERT(ptr && bytes == block_size_v);
            PPR_ASSERT(alignment <= AlignmentV);

            if (m_mru.isFull()) [[unlikely]] {
                void *const p_oldest = m_mru.popFrontAssumeNotEmpty();
                AllocatorT::deallocateRaw(p_oldest, bytes, AlignmentV);
            }

            poisonDestroyed(ptr, block_size_v);
            m_mru.pushBackAssumeNotFull(ptr);
        }
    };

    // ------------------------------------------------------------------
    // pooling allocator for fixed-size blocks with out-of-band metadata
    // ------------------------------------------------------------------

    template<std::size_t BlockSizeV,
        details::TAllocator BlockAllocatorT,
        std::size_t MaxNumBlocksV,
        std::align_val_t AlignmentV = max_align_v>
        requires (BlockSizeV > 0u && MaxNumBlocksV > 0u && (MaxNumBlocksV & bit_count_v<std::size_t>) == 0u)
    class Pooling : BlockAllocatorT {
        using page_mask_t = std::size_t;
        static_assert(std::atomic<page_mask_t>::is_always_lock_free);

    public:
        static constexpr std::size_t num_pools_v = divideRoundUp(MaxNumBlocksV, bit_count_v<page_mask_t>);
        static constexpr std::size_t block_size_v = BlockSizeV;
        static constexpr std::size_t pool_size_v = BlockSizeV * Bitmask<page_mask_t>::bit_count_v;

    private:
        [[nodiscard]] void *allocateLockFree_(const u32 pool_index) noexcept {
            Bitmask<page_mask_t> expected_blocks(m_blocks[pool_index]);
            while (expected_blocks.any()) {
                Bitmask<page_mask_t> desired_blocks(expected_blocks);
                const u32 block_index = desired_blocks.popAssumeNotEmpty();

                if (const std::atomic_ref<page_mask_t> atomic_blocks(std::ref(m_blocks[pool_index]));
                    atomic_blocks.compare_exchange_strong(expected_blocks.m_bits, desired_blocks.m_bits,
                                                          std::memory_order_acquire, std::memory_order_relaxed)) [[likely]] {
                    PPR_ASSERT(m_pools[pool_index]);
                    m_num_free_blocks.fetch_sub(1u);
                    return m_pools[pool_index] + block_index * block_size_v;
                }
            }

            return nullptr;
        }

        void deallocateLockFree_(const u32 pool_index, const void *const ptr) noexcept {
            const u32 block_index = checked_cast<u32>((static_cast<const std::byte *>(ptr) - m_pools[pool_index]) / block_size_v);
            PPR_ASSERT(Bitmask<page_mask_t>::bit_count_v > block_index);
            PPR_ASSERT(Bitmask<page_mask_t>{m_blocks[pool_index]}.test(block_index) == false);

            // release the block and check if the pool needs to be released
            const std::atomic_ref<page_mask_t> atomic_blocks(std::ref(m_blocks[pool_index]));
            const page_mask_t block_mask = static_cast<page_mask_t>(1u) << block_index;
            const page_mask_t prev_mask = atomic_blocks.fetch_or(block_mask);

            // if pool is fully unused and more than 1 pool is alive, try to release this pool
            const std::size_t prev_num_free_blocks = m_num_free_blocks.fetch_add(1u);
            if (prev_num_free_blocks > Bitmask<page_mask_t>::bit_count_v &&
                (prev_mask | block_mask) == Bitmask<page_mask_t>::all_v) [[unlikely]] {
                deallocateRawReleasePool_(pool_index);
            }
        }

        [[nodiscard]] PPR_NO_INLINE void *allocateRawFallback_(u32 *p_hint) {
            const std::lock_guard lock_for_pools{m_barrier_for_pools};

            // check if the pools is still empty to avoid race-conditions
            if (m_num_free_blocks.load(std::memory_order_relaxed) != 0u) {
                for (u32 pool_index = 0u; pool_index < m_pools.size(); pool_index++) {
                    if (void *const pool_block = allocateLockFree_(pool_index);
                        pool_block != nullptr) {
                        if (p_hint != nullptr) {
                            *p_hint = pool_index;
                        }
                        return pool_block;
                    }
                }
            }

            // look for the first unused pool index
            u32 free_pool_index = umax_v;
            for (u32 pool_index = 0u; pool_index < m_pools.size(); pool_index++) {
                if (m_pools[pool_index] == nullptr) {
                    free_pool_index = pool_index;
                    break;
                }
            }
            if (free_pool_index >= m_pools.size()) {
                throw std::bad_alloc();
            }

            // now we can safely assume that a new pool is indeed needed
            PPR_ASSERT(m_pools[free_pool_index] == nullptr);
            PPR_ASSERT(m_blocks[free_pool_index] == 0u);

            m_pools[free_pool_index] = static_cast<std::byte *>(BlockAllocatorT::allocateRaw(pool_size_v, AlignmentV).ptr);
            if (m_pools[free_pool_index] == nullptr) {
                throw std::bad_alloc();
            }

            constexpr u32 new_block_index = 0u;
            void *const new_block = static_cast<std::byte *>(m_pools[free_pool_index]) + new_block_index * block_size_v;
            constexpr Bitmask<page_mask_t> new_blocks(Bitmask<page_mask_t>::all_v & ~static_cast<page_mask_t>(1u));
            PPR_ASSERT(new_blocks.test(new_block_index) == false);

            const std::atomic_ref<page_mask_t> atomic_blocks(std::ref(m_blocks[free_pool_index]));
            atomic_blocks.store(new_blocks.m_bits);

            if (p_hint != nullptr) {
                *p_hint = free_pool_index;
            }

            m_num_free_blocks.fetch_add(Bitmask<page_mask_t>::bit_count_v - 1u);
            return new_block;
        }

        PPR_NO_INLINE void deallocateRawReleasePool_(const u32 pool_index) noexcept {
            const std::lock_guard lock_for_pools{m_barrier_for_pools};

            // reserve all the blocks at *once* to avoid other thread from touching this pool
            constexpr Bitmask<page_mask_t> all_blocks_reserved{0u};
            Bitmask<page_mask_t> expected_blocks{Bitmask<page_mask_t>::all_v};
            const std::atomic_ref<page_mask_t> atomic_blocks(std::ref(m_blocks[pool_index]));

            if (not atomic_blocks.compare_exchange_strong(
                expected_blocks.m_bits, all_blocks_reserved.m_bits,
                std::memory_order_acquire, std::memory_order_relaxed)) [[unlikely]] {
                return;
            }

            // finally, it's safe to release the memory since the block is completely allocated by us
            PPR_ASSERT(m_blocks[pool_index] == 0u);
            BlockAllocatorT::deallocateRaw(m_pools[pool_index], pool_size_v, AlignmentV);
            m_pools[pool_index] = nullptr;

            m_num_free_blocks.fetch_sub(Bitmask<page_mask_t>::bit_count_v);
        }

        alignas(hal::cacheline_size_v) std::mutex m_barrier_for_pools{};

        [[maybe_unused]]
        const std::byte m_padding_for_alignment[hal::cacheline_size_v - sizeof(m_barrier_for_pools) % hal::cacheline_size_v]{};

        std::atomic<std::size_t> m_num_free_blocks{0u};

        std::array<page_mask_t, num_pools_v> m_blocks{};
        std::array<std::byte *, num_pools_v> m_pools{};

    public:
        constexpr Pooling() noexcept(std::is_nothrow_constructible_v<BlockAllocatorT>) = default;

        explicit constexpr Pooling(const BlockAllocatorT &alloc)
            noexcept(std::is_nothrow_copy_constructible_v<BlockAllocatorT>)
            : BlockAllocatorT(alloc) {
        }

        explicit constexpr Pooling(BlockAllocatorT &&alloc)
            noexcept(std::is_nothrow_move_constructible_v<BlockAllocatorT>)
            : BlockAllocatorT(std::move(alloc)) {
        }

        constexpr ~Pooling() noexcept {
            const std::lock_guard lock_for_pools{m_barrier_for_pools};

            for (u32 pool_index = 0; pool_index < num_pools_v; pool_index++) {
                if (m_pools[pool_index] == nullptr) {
                    continue;
                }

                PPR_ASSERT(Bitmask<page_mask_t>(m_blocks[pool_index]).all() &&
                    "Destroying a Pooling allocator with live allocations");

                BlockAllocatorT::deallocateRaw(m_pools[pool_index], pool_size_v, AlignmentV);
                m_pools[pool_index] = nullptr;
                m_blocks[pool_index] = 0u;
            }
        }

        [[nodiscard]] constexpr bool owns(const void *const ptr,
                                          const std::size_t size,
                                          u32 *p_hint = nullptr) const noexcept {
            PPR_ASSERT(ptr != nullptr);

            if (p_hint && *p_hint < num_pools_v &&
                overlap(m_pools[*p_hint], pool_size_v, ptr)) [[likely]] {
                return true;
            }

            return std::ranges::any_of(m_pools, [ptr, size](const std::byte *const address) constexpr noexcept {
                return overlap(address, pool_size_v, ptr, size);
            });
        }

        [[nodiscard]] constexpr std::allocation_result<void *>
        allocateRaw([[maybe_unused]] const std::size_t bytes,
                    [[maybe_unused]] const std::align_val_t alignment,
                    u32 *p_hint = nullptr) noexcept {
            PPR_ASSERT(bytes == block_size_v);
            PPR_ASSERT(alignForward(static_cast<std::size_t>(alignment), block_size_v) == block_size_v);

            void *pool_block = nullptr;

            if (m_num_free_blocks.load(std::memory_order_relaxed) > 0u) {
                if (p_hint && *p_hint < num_pools_v && m_blocks[*p_hint]) [[likely]] {
                    pool_block = allocateLockFree_(*p_hint);
                    if (pool_block != nullptr) [[likely]] {
                        goto RETURN_BLOCK;
                    }
                }

                for (u32 pool_index = 0u; pool_index < m_pools.size(); pool_index++) {
                    if (m_pools[pool_index] == nullptr) {
                        continue;
                    }

                    pool_block = allocateLockFree_(pool_index);
                    if (pool_block != nullptr) [[likely]] {
                        if (p_hint) {
                            *p_hint = pool_index;
                        }
                        goto RETURN_BLOCK;
                    }
                }
            }

            pool_block = allocateRawFallback_(p_hint);

        RETURN_BLOCK:
            PPR_ASSERT(pool_block != nullptr);
            PPR_ASSERT(alignForward(pool_block, alignment) == pool_block);
            unpoisonUninitialized(pool_block, block_size_v);
            return {pool_block, block_size_v};
        }

        constexpr void deallocateRaw(const void *const ptr,
                                     [[maybe_unused]] const std::size_t bytes,
                                     [[maybe_unused]] const std::align_val_t alignment,
                                     u32 *p_hint = nullptr) noexcept {
            PPR_ASSERT(ptr != nullptr);
            PPR_ASSERT(bytes == block_size_v);
            PPR_ASSERT(alignForward(static_cast<std::size_t>(alignment), block_size_v) == block_size_v);
            PPR_ASSUME(ptr != nullptr);

            poisonDestroyed(const_cast<void *>(ptr), block_size_v);

            if (p_hint && *p_hint < num_pools_v && overlap(m_pools[*p_hint], pool_size_v, ptr)) {
                deallocateLockFree_(*p_hint, ptr);
                return;
            }

            for (u32 pool_index = 0u; pool_index < m_pools.size(); pool_index++) {
                if (overlap(m_pools[pool_index], pool_size_v, ptr)) {
                    if (p_hint) {
                        *p_hint = pool_index;
                    }
                    deallocateLockFree_(pool_index, ptr);
                    return;
                }
            }

            PPR_ASSERT(false && "Trying to deallocate a pointer outside of the pool");
            std::unreachable();
        }
    };

    namespace details {
        template<typename T>
        concept TPoolingHint = requires()
        {
            { T::value } -> std::convertible_to<u32 &>;
        };
    }

    template<std::size_t BlockSizeV,
        details::TAllocator BlockAllocatorT,
        std::size_t MaxNumBlocksV,
        details::TPoolingHint PoolingHintT,
        std::align_val_t AlignmentV = max_align_v>
        requires (BlockSizeV > 0u && MaxNumBlocksV > 0u && (MaxNumBlocksV & bit_count_v<std::size_t>) == 0u)
    class HintedPooling : Pooling<BlockSizeV, BlockAllocatorT, MaxNumBlocksV, AlignmentV> {
        using super_t = Pooling<BlockSizeV, BlockAllocatorT, MaxNumBlocksV, AlignmentV>;

    public:
        using super_t::super_t;
        using super_t::num_pools_v;
        using super_t::pool_size_v;
        using super_t::block_size_v;

        [[nodiscard]] PPR_FORCE_INLINE static constexpr u32 *getHintPtr() noexcept {
            return std::addressof(PoolingHintT::value);
        }

        [[nodiscard]] constexpr bool owns(const void *const ptr, const std::size_t bytes) {
            return super_t::owns(ptr, bytes, getHintPtr());
        }

        [[nodiscard]] constexpr std::allocation_result<void *> allocateRaw(const std::size_t bytes, const std::align_val_t alignment) {
            return super_t::allocateRaw(bytes, alignment, getHintPtr());
        }

        constexpr void deallocateRaw(const void *const ptr, const std::size_t bytes, const std::align_val_t alignment) {
            return super_t::deallocateRaw(ptr, bytes, alignment, getHintPtr());
        }
    };

    // ------------------------------------------------------------------
    // static allocator accessor from a callback, for "singleton"
    // ------------------------------------------------------------------

    template<details::TAllocator AllocatorT, AllocatorT &(*GetAllocatorF)() noexcept>
    class Accessor {
    public:
        [[nodiscard]] PPR_FORCE_INLINE
        static constexpr std::allocation_result<void *>
        allocateRaw(const std::size_t bytes, const std::align_val_t alignment) noexcept {
            return GetAllocatorF().allocateRaw(bytes, alignment);
        }

        PPR_FORCE_INLINE
        static constexpr void
        deallocateRaw(const void *const ptr,
                      const std::size_t bytes,
                      const std::align_val_t alignment) noexcept {
            return GetAllocatorF().deallocateRaw(ptr, bytes, alignment);
        }

        [[nodiscard]] PPR_FORCE_INLINE
        static constexpr bool
        owns(const void *const ptr, const std::size_t size) noexcept
            requires details::TOwningAllocator<AllocatorT> {
            return GetAllocatorF().owns(ptr, size);
        }

        [[nodiscard]] PPR_FORCE_INLINE
        static constexpr bool
        resizeRaw(const void *const ptr, const std::size_t old_size, const std::size_t new_size) noexcept
            requires details::TResizableAllocator<AllocatorT> {
            return GetAllocatorF().resizeRaw(ptr, old_size, new_size);
        }
    };

    template<auto GetAllocatorF>
        requires details::TAllocator<decltype(GetAllocatorF())>
    using Static = Accessor<std::remove_cvref_t<decltype(GetAllocatorF())>, GetAllocatorF>;

    // ------------------------------------------------------------------
    // STL allocator wrapper around Allocator<>
    // ------------------------------------------------------------------

    template<typename T, details::TAllocator AllocatorT>
    class STL : Allocator<AllocatorT> {
        template<typename, details::TAllocator>
        friend class STL;

    public:
        using allocator_type = Allocator<AllocatorT>;
        using value_type = T;
        using size_type = std::size_t;
        using difference_type = std::ptrdiff_t;
        using propagate_on_container_move_assignment = std::true_type;

        using allocator_type::allocator_type;
        using allocator_type::operator=;

        template<typename U>
        explicit constexpr STL(const STL<U, AllocatorT> &other)
            noexcept(std::is_nothrow_copy_constructible_v<allocator_type>)
            requires std::is_copy_constructible_v<allocator_type>
            : allocator_type(other) {
        }

        template<typename U>
        constexpr STL &operator=(const STL<U, AllocatorT> &other)
            noexcept(std::is_nothrow_copy_assignable_v<allocator_type>)
            requires std::is_copy_assignable_v<allocator_type> {
            allocator_type::operator=(other);
            return *this;
        }

        template<typename U>
        explicit constexpr STL(STL<U, AllocatorT> &&other)
            noexcept(std::is_nothrow_move_constructible_v<allocator_type>)
            requires std::is_move_constructible_v<allocator_type>
            : allocator_type(std::move(other)) {
        }

        template<typename U>
        constexpr STL &operator=(STL<U, AllocatorT> &&other)
            noexcept(std::is_nothrow_move_assignable_v<allocator_type>)
            requires std::is_move_assignable_v<allocator_type> {
            allocator_type::operator=(std::move(other));
            return *this;
        }

        [[nodiscard]] constexpr
        value_type *allocate(const size_type n) {
            return allocator_type::template allocate<value_type>(n);
        }

        [[nodiscard]] constexpr
        std::allocation_result<value_type *> allocate_at_least(const size_type n) {
            return allocator_type::template allocate_at_least<value_type>(n);
        }

        constexpr void deallocate(value_type *ptr, const size_type n) {
            return allocator_type::template deallocate<value_type>(ptr, n);
        }
    };

    template<typename T, details::TAllocator AllocatorT>
    struct details::use_inplace<STL<T, AllocatorT> > : use_inplace<Allocator<AllocatorT> > {
    };
}

// ------------------------------------------------------------------
// custom placement new and delete operators for Arena<>
// ------------------------------------------------------------------

// Need to be defined in Global namespace
export template<pP::mem::details::TArenaAllocator ArenaT>
[[nodiscard]] PPR_FORCE_INLINE void *operator new(const std::size_t size_bytes, ArenaT &arena) noexcept {
    return arena.allocateRaw(size_bytes, pP::max_align_v).ptr;
}

// Note that this operator can't be called explicitly,
// and will be called only if an exception is thrown in the placement new above.
// https://isocpp.org/wiki/faq/dtors#placement-delete
export template<pP::mem::details::TArenaAllocator ArenaT>
void operator delete(void *ptr, std::size_t size_bytes, ArenaT &arena) {
    arena.deallocateRaw(ptr, size_bytes, pP::max_align_v);
}
