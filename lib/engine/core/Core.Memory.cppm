module;
#include "pP/Macros.h"

export module engine.core:memory;

import :assert;
import :containers;
import :hal;
import :page_pool;

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
        std::align_val_t Alignment = alignof_v<T>,
        std::unsigned_integral Size_T = std::size_t>
    class Allocation {
    public:
        static_assert(alignof(T) <= static_cast<std::size_t>(Alignment));
        static_assert(std::is_same_v<std::remove_cvref_t<T>, T>);

        std::allocation_result<void *, Size_T> m_block{};

        static constexpr bool is_stateless_v = std::is_empty_v<AllocatorT>;

        constexpr Allocation() noexcept = default;

        constexpr Allocation(Allocation &&rvalue) noexcept
            : m_block(std::move(rvalue.m_block)) {
            rvalue.m_block = {};
        }

        // can't implement since the allocator is not accessible here
        Allocation &operator =(Allocation &&rvalue) noexcept {
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
            return Alignment;
        }

        [[nodiscard]] constexpr bool owns(const void *const ptr, const std::size_t size) const noexcept {
            return overlap(data(), size_bytes(), ptr, size);
        }

        [[nodiscard]] PPR_FORCE_INLINE constexpr std::span<T> view() const noexcept {
            PPR_ASSERT(isValid() && "Allocation is not valid");
            return std::span<T>(data(), count());
        }

        [[maybe_unused]] constexpr std::allocation_result<T *>
        allocate(AllocatorT &al, const std::size_t n = 1u) noexcept {
            PPR_ASSERT(!isValid() && "Allocation is already valid");
            const std::allocation_result<void *> blk = al.allocateRaw(sizeof(T) * n, Alignment);
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

        [[nodiscard]] constexpr bool resize(AllocatorT &al, const std::size_t new_size) noexcept
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

        [[maybe_unused]] constexpr std::allocation_result<T *> relocate(AllocatorT &al, const std::size_t new_size) noexcept
            requires pP::details::is_relocatable_v<T> {
            if (m_block.ptr) {
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
                    std::allocation_result<void *> new_block = al.allocateRaw(raw_size, Alignment);

                    std::memcpy(new_block.ptr, m_block.ptr, std::min(raw_size, m_block.count));
                    al.deallocateRaw(m_block.ptr, m_block.count, Alignment);

                    m_block = std::move(new_block);
                }
                return {static_cast<T *>(m_block.ptr), m_block.count / sizeof(T)};
            }
            return allocate(new_size);
        }

        [[maybe_unused]] constexpr std::allocation_result<T *> relocate(const std::size_t new_size) noexcept
            requires is_stateless_v && pP::details::is_relocatable_v<T> {
            AllocatorT al{};
            return relocate(al, new_size);
        }

        constexpr void deallocate(AllocatorT &al) noexcept {
            if (isValid()) [[likely]] {
                al.deallocateRaw(m_block.ptr, m_block.count, Alignment);
                m_block = {};
            }
        }

        constexpr void deallocate() noexcept requires is_stateless_v {
            AllocatorT al{};
            deallocate(al);
        }

        constexpr void deallocateAssumeNotEmpty(AllocatorT &al) noexcept {
            al.deallocateRaw(m_block.ptr, m_block.count, Alignment);
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
                al.deallocateRaw(m_block.ptr, m_block.count, Alignment);
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
        static constexpr bool is_stateless_v = details::TAllocator<AllocatorT> && std::is_empty_v<AllocatorT>;

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
        // non-trivial objects
        // ------------------------------------------------------------------

        template<typename T, typename... ArgsT> requires std::is_constructible_v<T, ArgsT &&...>
        [[nodiscard]] constexpr T *create(this AllocatorT &al, ArgsT &&... args) noexcept
            requires details::TAllocator<AllocatorT> {
            if (T *const ptr = al.template allocate<T>()) [[likely]]{
                return std::construct_at(ptr, std::forward<ArgsT>(args)...);
            }
            return nullptr;
        }

        template<typename T> requires std::is_destructible_v<T>
        constexpr void destroy(this AllocatorT &al, T *const ptr) noexcept
            requires details::TAllocator<AllocatorT> {
            if (ptr) [[likely]] {
                std::destroy_at(ptr);
                al.deallocate(ptr);
            }
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
        // trivial range
        // ------------------------------------------------------------------

        template<typename T> requires std::is_trivially_destructible_v<T>
        [[nodiscard]] constexpr std::span<T>
        span(this AllocatorT &al, const std::size_t n, const std::align_val_t alignment = alignof_v<T>) noexcept
            requires details::TAllocator<AllocatorT> {
            const std::allocation_result<T *> block = al.template allocate_at_least<T>(n, alignment);
            return std::span<T>(block.ptr, block.count);
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
    };

    namespace details {
        template<TAllocator>
        struct use_inplace : std::false_type {
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
                std::is_empty_v<AllocatorT> || details::use_inplace_v<AllocatorT>,
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

        constexpr Allocator(AllocatorT &value)
            noexcept(std::is_nothrow_constructible_v<reference, AllocatorT &>)
            requires std::is_constructible_v<reference, AllocatorT &>
            : reference(value) {
        }

        constexpr Allocator(const AllocatorT &value)
            noexcept(std::is_nothrow_constructible_v<reference, const AllocatorT &>)
            requires std::is_constructible_v<reference, const AllocatorT &>
            : reference(value) {
        }

        template<typename... ArgsT>
        explicit constexpr Allocator([[maybe_unused]] std::in_place_t _, ArgsT &&... args)
            noexcept(std::is_nothrow_constructible_v<reference, ArgsT &&...>)
            requires std::is_constructible_v<reference, ArgsT &&...>
            : reference(std::forward<ArgsT>(args)...) {
        }

        constexpr Allocator(const Allocator &) noexcept = default;

        constexpr Allocator &operator=(const Allocator &) noexcept = default;

        constexpr Allocator(Allocator &&) noexcept = default;

        constexpr Allocator &operator=(Allocator &&) noexcept = default;

        [[nodiscard]] constexpr operator AllocatorT &() noexcept {
            return materialize();
        }

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

        constexpr void deallocateRaw(void *const ptr, const std::size_t bytes, const std::align_val_t alignment = max_align_v) noexcept {
            materialize().deallocateRaw(ptr, bytes, alignment);
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
    // general purpose allocator use stl default allocator
    // ------------------------------------------------------------------

    class GPA {
    public:
        [[nodiscard]] static std::allocation_result<void *>
        allocateRaw(const std::size_t bytes, const std::align_val_t alignment) noexcept {
            void *const ptr = (alignment > max_align_v ? operator new(bytes, alignment, std::nothrow) : operator new(bytes, std::nothrow));
            PPR_ASSERT(!ptr || alignForward(ptr, alignment) == ptr);
            return {ptr, ptr ? bytes : 0u};
        }

        static void deallocateRaw(void *const ptr, const std::size_t bytes, const std::align_val_t alignment) noexcept {
            if (alignment > max_align_v) {
                operator delete(ptr, bytes, alignment);
            } else {
                operator delete(ptr, bytes);
            }
        }
    };

    // ------------------------------------------------------------------
    // os virtual memory allocator
    // ------------------------------------------------------------------

    class OS {
    public:
        [[nodiscard]] static std::allocation_result<void *>
        allocateRaw(const std::size_t bytes, const std::align_val_t alignment) {
            const auto [ptr, reserved] = hal::pageAlloc(bytes);
            PPR_ASSERT(std::bit_cast<std::uintptr_t>(ptr) % static_cast<std::size_t>(alignment) == 0);
            return {ptr, reserved};
        }

        static void deallocateRaw(void *const ptr, const std::size_t bytes, [[maybe_unused]] const std::align_val_t alignment) {
            hal::pageFree(ptr, bytes);
        }
    };

    // ------------------------------------------------------------------
    // pooled 2mib "huge" pages, serves as backend allocator
    // ------------------------------------------------------------------

    class HugePage {
    public:
        static constexpr std::size_t page_size = 2ull << 20u; // 2.0 MiB
        static constexpr std::size_t reserved_size = 16ull << 30u; // 16.0 GiB
        static constexpr std::size_t num_reserved_pages = reserved_size / page_size;

        [[nodiscard]] static PagePool &globalPool() noexcept {
            static PagePool g_instance{page_size, num_reserved_pages};
            return g_instance;
        }

        using ThreadLocalHint = PagePool::Hint<2u>;

        [[nodiscard]] static ThreadLocalHint &threadLocalHint() noexcept {
            thread_local ThreadLocalHint g_instance_tls{globalPool()};
            return g_instance_tls;
        }

        [[nodiscard]] static std::allocation_result<void *>
        allocateRaw(const std::size_t bytes, const std::align_val_t alignment) {
            return threadLocalHint().allocateRaw(bytes, alignment);
        }

        static void deallocateRaw(void *const ptr, const std::size_t bytes, const std::align_val_t alignment) {
            threadLocalHint().deallocateRaw(ptr, bytes, alignment);
        }
    };

    // ------------------------------------------------------------------
    // fallback to secondary if primary fails, and deallocate from the owning allocator
    // ------------------------------------------------------------------

    template<details::TOwningAllocator PrimaryT, details::TAllocator SecondaryT>
    class PPR_EMPTY_BASES Fallback : PrimaryT, SecondaryT {
    public:
        constexpr Fallback() requires std::is_default_constructible_v<PrimaryT> &&
                                      std::is_default_constructible_v<SecondaryT> = default;

        explicit constexpr Fallback(PrimaryT &&primary)
            noexcept(std::is_nothrow_move_constructible_v<PrimaryT> && std::is_nothrow_default_constructible_v<SecondaryT>)
            requires std::is_move_constructible_v<PrimaryT> && std::is_default_constructible_v<SecondaryT>
            : PrimaryT(std::move(primary)) {
        }

        explicit constexpr Fallback(SecondaryT &&secondary)
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

        explicit constexpr Fallback(const PrimaryT &primary)
            noexcept(std::is_nothrow_copy_constructible_v<PrimaryT> && std::is_nothrow_default_constructible_v<SecondaryT>)
            requires std::is_copy_constructible_v<PrimaryT> && std::is_default_constructible_v<SecondaryT>
            : PrimaryT(primary) {
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

    template<details::TAllocator UnderT, std::size_t SizeThreshold, details::TAllocator AboveT>
    class Threshold : UnderT, AboveT {
    public:
        static constexpr std::size_t size_threshold = SizeThreshold;

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

    template<std::size_t InSituSize, std::align_val_t Alignment = max_align_v>
    class InSitu {
        enum EStatus_ : u8 {
            status_free_ = 0x7F,
            status_used_ = 0x7A,
        };

        alignas(static_cast<std::size_t>(Alignment)) std::byte m_storage[InSituSize];
        EStatus_ m_status{status_free_};

    public:
#if PPR_ENABLE_ASSERTIONS
        constexpr InSitu() noexcept { // NOLINT(*-pro-type-member-init)
            poison(Poison::reserved, m_storage, sizeof(m_storage));
        }

        constexpr ~InSitu() noexcept {
            PPR_ASSERT(m_status == status_free_ && "In-situ buffer is still in use during destruction");
            m_status = status_used_; // disable use-after-free
            poison(Poison::destroyed, m_storage, sizeof(m_storage));
        }
#endif

        [[nodiscard]] constexpr bool owns(const void *const ptr, const std::size_t size) const noexcept {
            const bool owned = overlap(m_storage, InSituSize, ptr, size);
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
            poisonIfDebug(Poison::uninitialized, aligned_ptr, space);
            return {aligned_ptr, space};
        }

        constexpr void deallocateRaw([[maybe_unused]] const void *const ptr,
                                     [[maybe_unused]] const std::size_t bytes,
                                     [[maybe_unused]] const std::align_val_t alignment) noexcept {
            PPR_ASSERT(m_status == status_used_ && "Trying to deallocate an in-situ buffer that isn't exhausted");
            PPR_ASSERT(overlap(m_storage, InSituSize, ptr, bytes) && "Trying to deallocate a pointer outside of the in-situ buffer");

            m_status = status_free_;
            poisonIfDebug(Poison::destroyed, static_cast<void *>(&m_storage), sizeof(m_storage));
        }

        [[nodiscard]] constexpr bool
        resizeRaw(const void *const ptr, [[maybe_unused]] const std::size_t old_size, const std::size_t new_size) noexcept {
            PPR_ASSERT(ptr && m_status == status_used_);
            PPR_ASSERT(overlap(m_storage, InSituSize, ptr, old_size) && "Trying to resize a pointer outside of the in-situ buffer");
            PPR_ASSUME(ptr != nullptr);
            return (static_cast<const std::byte *>(ptr) + new_size <= static_cast<const std::byte *>(m_storage) + InSituSize);
        }
    };

    template<std::size_t InSituSize,
        details::TAllocator FallbackT,
        std::align_val_t Alignment = max_align_v>
        requires (InSituSize > 0)
    using InSituFallback = Inplace<Fallback<InSitu<InSituSize, Alignment>, FallbackT> >;

    template<std::size_t InSituSize,
        details::TAllocator FallbackT,
        std::align_val_t Alignment = max_align_v>
        requires (InSituSize > 0)
    using InSituThreshold = Inplace<Threshold<InSitu<InSituSize, Alignment>, InSituSize, FallbackT> >;

    // ------------------------------------------------------------------
    // polymorphic allocator uses runtime dispatch for allocation
    // ------------------------------------------------------------------

    class Pmr {
        struct VTable {
            std::allocation_result<void *> (*m_allocateRaw)(void *context, std::size_t bytes, std::align_val_t alignment){};

            void (*m_deallocateRaw)(void *context, void *ptr, std::size_t bytes, std::align_val_t alignment){};

            bool (*m_resizeRaw)(void *context, void *ptr, std::size_t old_size, std::size_t new_size){};
        };

        void *m_context{nullptr};
        const VTable *m_vtable{nullptr};

    public:
        template<details::TAllocator AllocatorT>
        // ReSharper disable once CppNonExplicitConvertingConstructor
        Pmr(AllocatorT = {}) noexcept requires std::is_empty_v<AllocatorT>
            : m_context(nullptr) {
            static constexpr VTable g_vtable{
                .m_allocateRaw = [](void *const, const std::size_t bytes, const std::align_val_t alignment) -> std::allocation_result<void *> {
                    return AllocatorT{}.allocateRaw(bytes, alignment);
                },
                .m_deallocateRaw = [](void *const, void *const ptr, const std::size_t bytes, const std::align_val_t alignment) {
                    AllocatorT{}.deallocateRaw(ptr, bytes, alignment);
                },
                .m_resizeRaw = [](void *, [[maybe_unused]] void *const ptr, [[maybe_unused]] const std::size_t old_size, [[maybe_unused]] const std::size_t new_size) -> bool {
                    if constexpr (details::TResizableAllocator<AllocatorT>) {
                        return AllocatorT{}.resizeRaw(ptr, old_size, new_size);
                    } else {
                        return false;
                    }
                },
            };
            m_vtable = &g_vtable;
        }

        template<details::TAllocator AllocatorT>
        // ReSharper disable once CppNonExplicitConvertingConstructor
        Pmr(AllocatorT &al) noexcept requires (!std::is_empty_v<AllocatorT>)
            : m_context(std::addressof(al)) {
            static constexpr VTable g_vtable{
                .m_allocateRaw = [](void *const context, const std::size_t bytes, const std::align_val_t alignment) -> std::allocation_result<void *> {
                    return static_cast<AllocatorT *>(context)->allocateRaw(bytes, alignment);
                },
                .m_deallocateRaw = [](void *const context, void *const ptr, const std::size_t bytes, const std::align_val_t alignment) {
                    static_cast<AllocatorT *>(context)->deallocateRaw(ptr, bytes, alignment);
                },
                .m_resizeRaw = []([[maybe_unused]] void *const context, [[maybe_unused]] void *const ptr, [[maybe_unused]] const std::size_t old_size, [[maybe_unused]] const std::size_t new_size) -> bool {
                    if constexpr (details::TResizableAllocator<AllocatorT>) {
                        return static_cast<AllocatorT *>(context)->resizeRaw(ptr, old_size, new_size);
                    } else {
                        return false;
                    }
                },
            };
            m_vtable = &g_vtable;
        }

        template<details::TAllocator AllocatorT>
        // ReSharper disable once CppNonExplicitConvertingConstructor
        Pmr(Allocator<AllocatorT> al) noexcept
            : Pmr(al.materialize()) {
        }

        [[nodiscard]] std::allocation_result<void *>
        allocateRaw(const std::size_t bytes, const std::align_val_t alignment) const {
            return m_vtable->m_allocateRaw(m_context, bytes, alignment);
        }

        void deallocateRaw(void *const ptr, const std::size_t bytes, const std::align_val_t alignment) const {
            m_vtable->m_deallocateRaw(m_context, ptr, bytes, alignment);
        }

        [[nodiscard]] bool
        resizeRaw(void *const ptr, const std::size_t old_size, const std::size_t new_size) const noexcept {
            return m_vtable->m_resizeRaw(m_context, ptr, old_size, new_size);
        }

        [[nodiscard]] friend bool operator==(const Pmr &a, const Pmr &b) noexcept = default;
    };

    template<>
    struct details::use_inplace<Pmr> : std::true_type {
    };
}
