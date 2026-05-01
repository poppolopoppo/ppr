module;
#include "pP/Macros.h"

export module engine.core:arena;

import :assert;
import :containers;
import :hal;
import :memory;
import :page_pool;

import std;

export namespace pP::mem {
    // ------------------------------------------------------------------
    // arena allocator is growable with fixed slab size, but not thread-safe
    // ------------------------------------------------------------------

    template<details::TAllocator AllocatorT = HugePage>

    class PPR_EMPTY_BASES Arena : public AllocatorTraits<Arena<AllocatorT> >, AllocatorT {
        static_assert(std::is_same_v<AllocatorT, std::remove_cvref_t<AllocatorT> >);

        struct SlabHeader {
            std::size_t m_size_bytes{0u};
            SlabHeader *m_next{nullptr};
        };

        static constexpr u32 slab_overhead_size = sizeof(SlabHeader);

        [[nodiscard]] PPR_FORCE_INLINE constexpr SlabHeader *rootSlab_() const noexcept {
            return static_cast<SlabHeader *>(m_slab);
        }

        void pushSlab_(const std::size_t wanted_size) {
            PPR_ASSERT(wanted_size > sizeof(SlabHeader));
            SlabHeader *const prev_slab = rootSlab_();

            const std::allocation_result<void *> al = AllocatorT::allocateRaw(wanted_size, max_align_v);
            PPR_ASSERT(al.ptr && "Failed to allocate memory for arena block");

            m_slab = al.ptr;
            m_capacity = checked_cast<u32>(al.count);
            m_offset = sizeof(SlabHeader);
            new(std::launder(rootSlab_())) SlabHeader{
                .m_size_bytes = m_capacity,
                .m_next = prev_slab,
            };
        }

        void popSlab_() {
            if (m_slab == nullptr) {
                return;
            }

            SlabHeader *const next_slab = rootSlab_()->m_next;

            AllocatorT::deallocateRaw(m_slab, m_capacity, max_align_v);
            m_slab = nullptr;
            m_capacity = 0u;
            m_offset = 0u;

            if (next_slab) {
                m_slab = next_slab;
                m_offset = slab_overhead_size;
                m_capacity = checked_cast<u32>(next_slab->m_size_bytes);
            }
        }

        [[nodiscard]] bool ownsExhausted_(const void *const ptr) const noexcept {
            for (SlabHeader *slab = rootSlab_(); slab != nullptr; slab = slab->m_next) {
                if (overlap(slab, slab->m_size_bytes, ptr)) [[likely]] {
                    return true;
                }
            }
            return false;
        }

        PPR_NO_INLINE void restoreExhausted_(const void *const mark) {
            popSlab_();

            while (m_slab) [[likely]] {
                if (overlap(m_slab, m_capacity, mark)) [[likely]] {
                    m_offset = checked_cast<u32>(
                        std::bit_cast<std::uintptr_t>(mark) -
                        std::bit_cast<std::uintptr_t>(m_slab));
                    PPR_ASSERT(m_offset >= sizeof(SlabHeader));
                    poisonIfDebug(Poison::reserved, static_cast<std::byte *>(m_slab) + m_offset, m_capacity - m_offset);
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
            m_offset = (m_slab ? slab_overhead_size : 0u);
            while (m_slab && rootSlab_()->m_next) [[likely]] {
                popSlab_();
            }
        }

        void reset(const std::size_t initial_capacity) noexcept {
            reset();

            if (m_slab == nullptr || m_capacity != initial_capacity) {
                popSlab_();
                pushSlab_(initial_capacity);
            }
        }

        [[nodiscard]] PPR_FORCE_INLINE bool owns(const void *const ptr, const std::size_t size) const noexcept {
            if (overlap(m_slab, m_capacity, ptr, size)) [[likely]] {
                return true;
            }
            return ownsExhausted_(ptr);
        }

        [[nodiscard]] std::allocation_result<void *>
        allocateRaw(const std::size_t bytes, const std::align_val_t alignment) noexcept {
        RETRY_ALLOC:
            const u32 prev_offset = m_offset;
            std::size_t space = m_capacity - prev_offset;
            void *aligned_ptr = static_cast<std::byte *>(m_slab) + prev_offset;

            if (std::align(static_cast<std::size_t>(alignment), bytes, aligned_ptr, space) == nullptr) [[unlikely]] {
                pushSlab_(std::max(m_capacity, static_cast<u32>(bytes + sizeof(SlabHeader))));
                goto RETRY_ALLOC;
            }

            m_offset = checked_cast<u32>(
                (static_cast<std::byte *>(aligned_ptr) - static_cast<std::byte *>(m_slab)) +
                static_cast<std::ptrdiff_t>(bytes));
            poisonIfDebug(Poison::uninitialized, static_cast<std::byte *>(aligned_ptr), bytes);
            return {aligned_ptr, bytes};
        }

        // Only valid if ptr was the most recent allocation
        [[nodiscard]] bool resizeRaw(void *const ptr, const std::size_t old_size, const std::size_t new_size) noexcept {
            PPR_ASSERT(owns(ptr, old_size) && "Trying to resize a pointer outside of the arena");

            // Only resizable if it was the last allocation
            const auto byte_ptr = static_cast<std::byte *>(ptr);
            if (byte_ptr + old_size != static_cast<std::byte *>(m_slab) + m_offset) [[unlikely]] {
                return false; // not the top, caller must allocate+copy
            }

            const u32 new_offset = checked_cast<u32>(
                (static_cast<std::byte *>(byte_ptr) - static_cast<std::byte *>(m_slab)) +
                static_cast<std::ptrdiff_t>(new_size));
            if (new_offset > m_capacity) [[unlikely]] {
                return false; // OOM - can't relocate to a new slab
            }

            m_offset = new_offset;
            if (new_size > old_size) {
                poisonIfDebug(Poison::uninitialized, byte_ptr + old_size, new_size - old_size);
            }
            return true;
        }

        // Only valid if ptr was the most recent allocation
        [[maybe_unused]] /*constexpr*/ bool deallocateRaw(void *const ptr, const std::size_t bytes, [[maybe_unused]] const std::align_val_t alignment) noexcept {
            PPR_ASSERT(owns(ptr, bytes) && "Trying to deallocate a pointer outside of the arena");
            poisonIfDebug(Poison::destroyed, ptr, bytes);

            // Verify ptr is actually the top of the arena
            if (const std::byte *byte_ptr = static_cast<std::byte *>(ptr);
                byte_ptr + bytes == static_cast<const std::byte *>(m_slab) + m_offset) [[likely]] {
                m_offset = checked_cast<u32>(byte_ptr - static_cast<const std::byte *>(m_slab));
                return true;
            }

            return false; // not the last allocation, refuse silently
        }

        // Checkpoint the current offset for cheap scope-level rewind
        [[nodiscard]] constexpr const void *watermark() const noexcept {
            return static_cast<const std::byte *>(m_slab) + m_offset;
        }

        // Rewind to a previous checkpoint — no destructor calls, O(1)
        constexpr void restore(const void *const mark) noexcept {
            if (m_slab == nullptr) {
                PPR_ASSERT(mark == nullptr);
                return;
            }

            if (overlap(m_slab, m_capacity, mark)) [[likely]] {
                m_offset = static_cast<u32>(static_cast<const std::byte *>(mark) - static_cast<std::byte *>(m_slab));
                poisonIfDebug(Poison::reserved, static_cast<std::byte *>(m_slab) + m_offset, m_capacity - m_offset);
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
        const void *m_watermark{};

        ScopedArena() = delete;

        explicit constexpr ScopedArena(ArenaT &arena) noexcept
            : m_arena{std::addressof(arena)},
              m_watermark{arena.watermark()} {
        }

        constexpr ScopedArena(const ScopedArena &) = delete;

        constexpr ScopedArena &operator =(const ScopedArena &) = delete;

        constexpr ScopedArena(ScopedArena &&other) noexcept {
            std::swap(m_arena, other.m_arena);
            std::swap(m_watermark, other.m_watermark);
        }

        constexpr ScopedArena &operator =(ScopedArena &&other) noexcept {
            if (this == &other) [[unlikely]] return;

            if (m_arena) {
                PPR_ASSERT(m_watermark);
                m_arena->restore(m_watermark);

                m_arena = nullptr;
            }
            m_watermark = nullptr;

            std::swap(m_arena, other.m_arena);
            std::swap(m_watermark, other.m_watermark);
            return *this;
        }

        constexpr ~ScopedArena() noexcept {
            if (m_arena) {
                PPR_ASSERT(m_watermark);
                m_arena->restore(m_watermark);
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
            m_arena->reset();
        }
    };

    template<details::TArenaAllocator ArenaT>
    ScopedArena(ArenaT &arena) -> ScopedArena<ArenaT>;

    // ------------------------------------------------------------------
    // default Arena for for small transient allocations (eg string formatting)
    // ------------------------------------------------------------------

    class ScratchPad {
        // not constructible locally
        [[nodiscard]] static constexpr Arena<SmallPage> &getArenaTLS_() noexcept {
            alignas(hal::cacheline_size_v) thread_local Arena<SmallPage> g_instance_tls{};
            return g_instance_tls;
        }

    public:
        constexpr ScratchPad() noexcept = default;

        using scoped_arena_t = ScopedArena<Arena<SmallPage>>;

        [[nodiscard]] static constexpr scoped_arena_t open() noexcept {
            return ScopedArena(getArenaTLS_());
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
}

// ------------------------------------------------------------------
// custom placement new and delete operators for Arena<>
// ------------------------------------------------------------------

// Need to be defined in Global namespace
export template<pP::mem::details::TAllocator AllocatorT>
[[nodiscard]] void *operator new(const std::size_t size_bytes, pP::mem::Arena<AllocatorT> &arena) noexcept {
    return arena.allocateRaw(size_bytes, pP::max_align_v).ptr;
}

// Note that this operator can't be called explicitly,
// and will be called only if an exception is thrown in the placement new above.
// https://isocpp.org/wiki/faq/dtors#placement-delete
export template<pP::mem::details::TAllocator AllocatorT>
void operator delete(void *ptr, std::size_t size_bytes, pP::mem::Arena<AllocatorT> &arena) {
    arena.deallocateRaw(ptr, size_bytes, pP::max_align_v);
}
