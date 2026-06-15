module;
#include "pP/Macros.h"

export module engine.core:memory.arena;

import :assert;
import :containers;
import :hal;
import :memory;
import :memory.poison;
import :memory.page_pool;

import std;

export namespace pP::mem {
    // ------------------------------------------------------------------
    // slab allocator has a single chunk of fixed size, which it does not own
    // ------------------------------------------------------------------

    class Slab : public AllocatorTraits<Slab> {
        std::byte *m_data{nullptr};
        u32 m_capacity{0u};
        u32 m_offset{0u};

    public:
        constexpr Slab(void *const data, const std::size_t capacity) noexcept
            : m_data(static_cast<std::byte *>(data)),
              m_capacity(safe_narrowing{capacity}) {
            PPR_ASSERT(m_data != nullptr || m_capacity == 0u);
        }

        PPR_FORCE_INLINE explicit constexpr Slab(const std::span<std::byte> storage) noexcept
            : Slab(storage.data(), storage.size_bytes()) {
        }

        PPR_FORCE_INLINE explicit constexpr Slab(const std::allocation_result<void *> allocation) noexcept
            : Slab(static_cast<std::byte *>(allocation.ptr), allocation.count) {
        }

        Slab(const Slab &) = delete;

        Slab &operator =(const Slab &) = delete;

        constexpr Slab(Slab &&other) noexcept {
            swap(*this, other);
        }

        constexpr Slab &operator =(Slab &&other) noexcept {
            m_data = nullptr;
            m_capacity = m_offset = 0u;
            swap(*this, other);
            return *this;
        }

        [[nodiscard]] std::allocation_result<void *> data() const noexcept {
            return std::allocation_result<void *>(m_data, m_capacity);
        }

        friend constexpr void swap(Slab &lhs, Slab &rhs) noexcept {
            std::swap(lhs.m_data, rhs.m_data);
            std::swap(lhs.m_capacity, rhs.m_capacity);
            std::swap(lhs.m_offset, rhs.m_offset);
        }

        void reset() noexcept;

        [[nodiscard]] PPR_FORCE_INLINE constexpr bool owns(const void *const ptr, const std::size_t size) const noexcept {
            return overlap(m_data, m_capacity, ptr, size);
        }

        [[nodiscard]] std::allocation_result<void *>
        allocateRaw(const std::size_t bytes, const std::align_val_t alignment) noexcept(false);

        [[nodiscard]] bool resizeRaw(void *const ptr, const std::size_t old_size, const std::size_t new_size) noexcept;

        [[maybe_unused]] bool deallocateRaw(void *const ptr, const std::size_t bytes, const std::align_val_t alignment) noexcept;

        // Checkpoint the current offset for cheap scope-level rewind
        [[nodiscard]] constexpr const void *watermark() const noexcept {
            return m_data != nullptr ? m_data + m_offset : nullptr;
        }

        // Rewind to a previous checkpoint — no destructor calls, O(1)
        void restore(const void *const mark) noexcept;
    };

    template<std::size_t CapacityV>
    class InSituSlab : public Slab {
        alignas(std::max_align_t) std::byte m_storage[CapacityV]{};

    public:
        constexpr InSituSlab() noexcept // NOLINT(*-use-equals-default)
            : Slab(m_storage) {
        }
    };

    // ------------------------------------------------------------------
    // arena allocator is growable with fixed slab size, but not thread-safe
    // ------------------------------------------------------------------

    template<details::TAllocator AllocatorT = HugePage>
    class PPR_EMPTY_BASES Arena : public AllocatorTraits<Arena<AllocatorT> >, AllocatorT {
        static_assert(std::is_same_v<AllocatorT, std::remove_cvref_t<AllocatorT> >);

        struct SlabHeader {
            SlabHeader *m_next{nullptr};
            u32 m_capacity{0u};
            u32 m_offset{0u};
        };

        static constexpr u32 slab_overhead_size = sizeof(SlabHeader);

        [[nodiscard]] PPR_FORCE_INLINE constexpr SlabHeader *rootSlab_() const noexcept {
            return static_cast<SlabHeader *>(m_slab);
        }

        void pushSlab_(const std::size_t wanted_size) {
            PPR_ASSERT(wanted_size > sizeof(SlabHeader));
            SlabHeader *const prev_slab = rootSlab_();
            if (prev_slab) {
                prev_slab->m_offset = m_offset;
            }

            const std::allocation_result<void *> al = AllocatorT::allocateRaw(wanted_size, max_align_v);
            PPR_ASSERT(al.ptr && "Failed to allocate memory for arena block");

            m_slab = al.ptr;
            m_capacity = checked_cast<u32>(al.count);
            m_offset = slab_overhead_size;
            static_assert(sizeof(SlabHeader) <= slab_overhead_size);
            new(std::launder(rootSlab_())) SlabHeader{
                .m_next = prev_slab,
                .m_capacity = m_capacity,
                .m_offset = m_offset
            };

            annotateContiguousContainer(
                static_cast<std::byte *>(m_slab),
                m_capacity, 0u, m_offset);
        }

        void popSlab_() {
            if (m_slab == nullptr) {
                return;
            }

            SlabHeader *const next_slab = rootSlab_()->m_next;

            poisonDestroyed(m_slab, m_capacity);
            AllocatorT::deallocateRaw(m_slab, m_capacity, max_align_v);
            m_slab = nullptr;
            m_capacity = 0u;
            m_offset = 0u;

            if (next_slab) {
                m_slab = next_slab;
                m_offset = next_slab->m_offset;
                m_capacity = next_slab->m_capacity;
            }
        }

        [[nodiscard]] bool ownsExhausted_(const void *const ptr) const noexcept {
            for (SlabHeader *slab = rootSlab_()->m_next; slab != nullptr; slab = slab->m_next) {
                if (overlap(slab, slab->m_offset, ptr)) [[likely]] {
                    return true;
                }
            }
            return false;
        }

        PPR_NO_INLINE void restoreExhausted_(const void *const mark) {
            popSlab_();

            while (m_slab) [[likely]] {
                if (overlap(m_slab, m_capacity, mark)) [[likely]] {
                    const u32 old_offset = m_offset;
                    m_offset = checked_cast<u32>(
                        std::bit_cast<std::uintptr_t>(mark) -
                        std::bit_cast<std::uintptr_t>(m_slab));
                    PPR_ASSERT(m_offset >= sizeof(SlabHeader));
                    PPR_ASSERT(m_offset <= old_offset);

                    annotateContiguousContainer(
                        static_cast<std::byte *>(m_slab),
                        m_capacity, old_offset, m_offset);
                    return;
                }
                popSlab_();
            }

            PPR_ASSERT(false && "Trying to restore to a mark outside of the arena");
            std::unreachable();
        }

        void *m_slab{nullptr};
        u32 m_capacity{0u};
        u32 m_offset{0u};

    public:
        Arena() requires details::TBlockAllocator<AllocatorT>
            : Arena(AllocatorT::block_size_v) {
        }

        explicit Arena(const std::size_t initial_capacity)
            requires std::is_default_constructible_v<AllocatorT> {
            reset(initial_capacity);
        }

        explicit Arena(const AllocatorT &al) noexcept
            requires std::is_copy_constructible_v<AllocatorT>
            : AllocatorT(al) {
        }

        Arena(const std::size_t initial_capacity, const AllocatorT &al) noexcept
            requires std::is_copy_constructible_v<AllocatorT>
            : AllocatorT(al) {
            reset(initial_capacity);
        }

        explicit Arena(AllocatorT &&al) noexcept
            requires std::is_move_constructible_v<AllocatorT>
            : AllocatorT(std::move(al)) {
        }

        Arena(const std::size_t initial_capacity, AllocatorT &&al) noexcept
            requires std::is_move_constructible_v<AllocatorT>
            : AllocatorT(std::move(al)) {
            reset(initial_capacity);
        }

        Arena(const Arena &) = delete;

        Arena &operator =(const Arena &) = delete;

        Arena(Arena &&rvalue) noexcept
            : AllocatorT(std::move(rvalue)),
              m_slab(rvalue.m_slab),
              m_capacity(rvalue.m_capacity),
              m_offset(rvalue.m_offset) {
            rvalue.m_slab = nullptr;
            rvalue.m_capacity = 0u;
            rvalue.m_offset = 0u;
        }

        Arena &operator =(Arena &&rvalue) noexcept {
            if (&rvalue == this) {
                return *this;
            }

            reset();
            popSlab_();

            AllocatorT::operator =(std::move(rvalue));

            m_slab = rvalue.m_slab;
            m_capacity = rvalue.m_capacity;
            m_offset = rvalue.m_offset;

            rvalue.m_slab = nullptr;
            rvalue.m_capacity = 0u;
            rvalue.m_offset = 0u;
            return *this;
        }

        ~Arena() noexcept {
            reset();
            popSlab_();
        }

        void reset() noexcept {
            if (m_slab == nullptr) {
                return;
            }

            annotateContiguousContainer(
                static_cast<std::byte *>(m_slab),
                m_capacity, m_offset, slab_overhead_size);

            m_offset = slab_overhead_size;

            while (rootSlab_()->m_next) {
                popSlab_();

                annotateContiguousContainer(
                    static_cast<std::byte *>(m_slab),
                    m_capacity, m_offset, slab_overhead_size);

                m_offset = slab_overhead_size;
            }
        }

        void reset(std::size_t initial_capacity) noexcept {
            reset();

            if constexpr (details::TBlockAllocator<AllocatorT>) {
                initial_capacity = std::max(initial_capacity, AllocatorT::block_size_v);
            }

            if (m_slab == nullptr || m_capacity != initial_capacity) {
                popSlab_();
                pushSlab_(initial_capacity);
            }
        }

        [[nodiscard]] PPR_FORCE_INLINE bool owns(const void *const ptr, const std::size_t size) const noexcept {
            PPR_ASSERT(ptr != nullptr && size > 0u);
            if (overlap(m_slab, m_capacity, ptr, size)) [[likely]] {
                return true;
            }
            if (m_slab != nullptr && rootSlab_()->m_next != nullptr) [[unlikely]] {
                return ownsExhausted_(ptr);
            }
            return false;
        }

        [[nodiscard]] std::allocation_result<void *>
        allocateRaw(const std::size_t bytes, const std::align_val_t alignment) noexcept {
        RETRY_ALLOC:
            std::size_t space = m_capacity - m_offset;
            void *aligned_ptr = static_cast<std::byte *>(m_slab) + m_offset;

            if (std::align(static_cast<std::size_t>(alignment), bytes, aligned_ptr, space) == nullptr) [[unlikely]] {
                pushSlab_(std::max(static_cast<std::size_t>(m_capacity), bytes + sizeof(SlabHeader)));
                goto RETRY_ALLOC;
            }

            const u32 old_offset = m_offset;
            m_offset = checked_cast<u32>(static_cast<std::ptrdiff_t>(bytes) +
                                         static_cast<std::byte *>(aligned_ptr) - static_cast<std::byte *>(m_slab));

            annotateContiguousContainer(
                static_cast<std::byte *>(m_slab),
                m_capacity, old_offset, m_offset);

            return {aligned_ptr, bytes};
        }

        // Only valid if ptr was the most recent allocation
        [[nodiscard]] bool resizeRaw(void *const ptr, const std::size_t old_size, const std::size_t new_size) noexcept {
            PPR_ASSERT(owns(ptr, old_size) && "Trying to resize a pointer outside of the arena");
            if (old_size == new_size) {
                return true;
            }

            // Only resizable if it was the last allocation
            const auto byte_ptr = static_cast<std::byte *>(ptr);
            if (byte_ptr + old_size != static_cast<std::byte *>(m_slab) + m_offset) [[unlikely]] {
                return false; // not the top, caller must allocate+copy
            }

            const u32 new_offset = checked_cast<u32>(
                static_cast<std::byte *>(byte_ptr) - static_cast<std::byte *>(m_slab) +
                static_cast<std::ptrdiff_t>(new_size));
            if (new_offset > m_capacity) [[unlikely]] {
                return false; // OOM - can't relocate to a new slab
            }

            annotateContiguousContainer(
                static_cast<std::byte *>(m_slab),
                m_capacity, m_offset, new_offset);

            m_offset = new_offset;
            return true;
        }

        // Only valid if ptr was the most recent allocation
        [[maybe_unused]] /*constexpr*/ bool deallocateRaw(void *const ptr, const std::size_t bytes, [[maybe_unused]] const std::align_val_t alignment) noexcept {
            PPR_ASSERT(owns(ptr, bytes) && "Trying to deallocate a pointer outside of the arena");

            // Verify ptr is actually the top of the arena
            if (const std::byte *byte_ptr = static_cast<std::byte *>(ptr);
                byte_ptr + bytes == static_cast<const std::byte *>(m_slab) + m_offset) [[likely]] {
                const u32 old_offset = m_offset;
                m_offset = checked_cast<u32>(byte_ptr - static_cast<const std::byte *>(m_slab));

                annotateContiguousContainer(
                    static_cast<std::byte *>(m_slab),
                    m_capacity, old_offset, m_offset);
                return true;
            }

            return false;
        }

        // Checkpoint the current offset for cheap scope-level rewind
        [[nodiscard]] constexpr const void *watermark() const noexcept {
            return m_slab != nullptr ? static_cast<const std::byte *>(m_slab) + m_offset : nullptr;
        }

        // Rewind to a previous checkpoint — no destructor calls, O(1)
        constexpr void restore(const void *const mark) noexcept {
            if (overlap(m_slab, m_capacity, mark)) [[likely]] {
                const u32 prev_offset = m_offset;
                m_offset = static_cast<u32>(static_cast<const std::byte *>(mark) -
                                            static_cast<std::byte *>(m_slab));

                annotateContiguousContainer(
                    static_cast<std::byte *>(m_slab),
                    m_capacity, prev_offset, m_offset);
                return;
            }

            if (mark == nullptr) [[unlikely]] {
                reset();
                return;
            }

            restoreExhausted_(mark);
        }
    };

    template<details::TAllocator AllocatorT>
    Arena(AllocatorT &&al) -> Arena<std::remove_cvref_t<AllocatorT> >;

    template<details::TAllocator AllocatorT>
    Arena(std::size_t initial_capacity, AllocatorT &&al) -> Arena<std::remove_cvref_t<AllocatorT> >;

    // ------------------------------------------------------------------
    // RAII wrapper to scope all the allocations made in the arena
    // ------------------------------------------------------------------

    template<details::TArenaAllocator ArenaT>
    struct [[nodiscard]] ScopedArena {
        ArenaT *m_arena{};
        const void *m_scope_offset{};

        ScopedArena() = delete;

        explicit constexpr ScopedArena(ArenaT &arena) noexcept
            : m_arena{std::addressof(arena)},
              m_scope_offset{arena.watermark()} {
        }

        constexpr ScopedArena(const ScopedArena &) = delete;

        constexpr ScopedArena &operator =(const ScopedArena &) = delete;

        constexpr ScopedArena(ScopedArena &&other) noexcept {
            std::swap(m_arena, other.m_arena);
            std::swap(m_scope_offset, other.m_scope_offset);
        }

        constexpr ScopedArena &operator =(ScopedArena &&other) noexcept {
            if (this == &other) [[unlikely]] return *this;

            if (m_arena) {
                PPR_ASSERT(m_scope_offset);
                m_arena->restore(m_scope_offset);
                m_arena = nullptr;
            }
            m_scope_offset = nullptr;

            std::swap(m_arena, other.m_arena);
            std::swap(m_scope_offset, other.m_scope_offset);
            return *this;
        }

        constexpr ~ScopedArena() noexcept {
            if (m_arena) [[likely]] {
                PPR_ASSERT(m_scope_offset);
                m_arena->restore(m_scope_offset);
            }
        }

        [[nodiscard]] PPR_FORCE_INLINE
        bool owns(const void *const ptr, const std::size_t size) noexcept {
            return m_arena->owns(ptr, size);
        }

        [[nodiscard]] PPR_FORCE_INLINE
        std::allocation_result<void *>
        allocateRaw(const std::size_t bytes, const std::align_val_t alignment) noexcept {
            return m_arena->allocateRaw(bytes, alignment);
        }

        // Only valid if ptr was the most recent allocation
        [[nodiscard]] PPR_FORCE_INLINE
        bool resizeRaw(void *const ptr, const std::size_t old_size, const std::size_t new_size) noexcept {
            return m_arena->resizeRaw(ptr, old_size, new_size);
        }

        // Only valid if ptr was the most recent allocation
        [[maybe_unused]] PPR_FORCE_INLINE
        bool deallocateRaw(void *const ptr, const std::size_t bytes, [[maybe_unused]] const std::align_val_t alignment) noexcept {
            return m_arena->deallocateRaw(ptr, bytes, alignment);
        }

        // Checkpoint the current offset for cheap scope-level rewind
        [[nodiscard]] PPR_FORCE_INLINE constexpr
        const void *watermark() const noexcept {
            return m_arena->watermark();
        }

        // Rewind to a previous checkpoint — no destructor calls, O(1)
        PPR_FORCE_INLINE constexpr
        void restore(const void *const mark) noexcept {
            m_arena->restore(mark);
        }

        PPR_FORCE_INLINE constexpr
        void reset() noexcept {
            m_arena->restore(m_scope_offset);
        }
    };

    template<details::TArenaAllocator ArenaT>
    ScopedArena(ArenaT &arena) -> ScopedArena<ArenaT>;

    // ------------------------------------------------------------------
    // ScratchPad — arena for transient allocations
    // ------------------------------------------------------------------

    class ScratchPad {
        [[nodiscard]] static Arena<SmallPage> &getArenaTLS_() noexcept;

    public:
        constexpr ScratchPad() noexcept = default;

#if PPR_ENABLE_DEBUG
        class [[nodiscard]] ScopedArenaWithDebug : public ScopedArena<Arena<SmallPage> > {
            static i32 &getDepthTLS() noexcept {
                thread_local i32 g_depth_tls{-1};
                return g_depth_tls;
            }

            i32 m_depth{-1};

        public:
            explicit ScopedArenaWithDebug(Arena<SmallPage> &arena) noexcept
                : ScopedArena(arena)
                  , m_depth(++getDepthTLS()) {
            }

            ~ScopedArenaWithDebug() noexcept {
                if (m_depth < 0)
                    return;
                i32 &g_depth_tls = getDepthTLS();
                PPR_ASSERT(g_depth_tls == m_depth);
                g_depth_tls = m_depth - 1u;
            }

            ScopedArenaWithDebug(const ScopedArenaWithDebug &) = delete;

            ScopedArenaWithDebug &operator=(const ScopedArenaWithDebug &) = delete;

            ScopedArenaWithDebug(ScopedArenaWithDebug &&other) noexcept
                : ScopedArena(std::move(other)),
                  m_depth(other.m_depth) {
                other.m_depth = -1;
            }

            ScopedArenaWithDebug &operator=(ScopedArenaWithDebug &&other) noexcept {
                if (m_depth >= 0) {
                    i32 &g_depth_tls = getDepthTLS();
                    PPR_ASSERT(g_depth_tls == m_depth);
                    g_depth_tls = m_depth - 1u;
                }

                ScopedArena::operator=(std::move(other));
                m_depth = other.m_depth;
                other.m_depth = -1;
                return *this;
            }
        };

        using scoped_arena_t = ScopedArenaWithDebug;
#else
        using scoped_arena_t = ScopedArena<Arena<SmallPage> >;
#endif

        [[nodiscard]] static constexpr scoped_arena_t open() noexcept {
            return scoped_arena_t(getArenaTLS_());
        }

        [[nodiscard]] PPR_FORCE_INLINE static
        bool owns(const void *const ptr, const std::size_t size) noexcept {
            return getArenaTLS_().owns(ptr, size);
        }

        [[nodiscard]] PPR_FORCE_INLINE static
        std::allocation_result<void *>
        allocateRaw(const std::size_t bytes, const std::align_val_t alignment) noexcept {
            return getArenaTLS_().allocateRaw(bytes, alignment);
        }

        // Only valid if ptr was the most recent allocation
        [[nodiscard]] PPR_FORCE_INLINE static
        bool resizeRaw(void *const ptr, const std::size_t old_size, const std::size_t new_size) noexcept {
            return getArenaTLS_().resizeRaw(ptr, old_size, new_size);
        }

        // Only valid if ptr was the most recent allocation
        [[maybe_unused]] PPR_FORCE_INLINE static
        bool deallocateRaw(void *const ptr, const std::size_t bytes, [[maybe_unused]] const std::align_val_t alignment) noexcept {
            return getArenaTLS_().deallocateRaw(ptr, bytes, alignment);
        }

        // Checkpoint the current offset for cheap scope-level rewind
        [[nodiscard]] PPR_FORCE_INLINE static
        const void *watermark() noexcept {
            return getArenaTLS_().watermark();
        }

        // Rewind to a previous checkpoint — no destructor calls, O(1)
        PPR_FORCE_INLINE static
        void restore(const void *const mark) noexcept {
            getArenaTLS_().restore(mark);
        }

        PPR_FORCE_INLINE static
        void reset() noexcept {
            getArenaTLS_().reset();
        }
    };

    static_assert(details::use_inplace_v<ScratchPad>);

    extern template class Arena<HugePage>;
    extern template class Arena<SmallPage>;
}
