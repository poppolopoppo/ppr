module;
#include "pP/Macros.h"

export module engine.core:memory;

import :memory.allocator;
import :hal;
import :memory.page_pool;

import std;

export namespace pP::mem {
    // ------------------------------------------------------------------
    // general purpose allocator use stl default allocator
    // ------------------------------------------------------------------

    class GPA {
    public:
        [[nodiscard]] static std::allocation_result<void *>
        allocateRaw(const std::size_t bytes, const std::align_val_t alignment) noexcept;

        static void deallocateRaw(void *const ptr, const std::size_t bytes, const std::align_val_t alignment) noexcept;
    };

    static_assert(details::use_inplace_v<GPA>);

    // ------------------------------------------------------------------
    // os virtual memory allocator
    // ------------------------------------------------------------------

    class OS {
    public:
        [[nodiscard]] static std::allocation_result<void *>
        allocateRaw(const std::size_t bytes, const std::align_val_t alignment);

        static void deallocateRaw(void *const ptr, const std::size_t bytes, const std::align_val_t alignment);
    };

    static_assert(details::use_inplace_v<OS>);
    static_assert(details::TAllocator<OS>);

    // ------------------------------------------------------------------
    // polymorphic allocator uses runtime dispatch for allocation
    // ------------------------------------------------------------------

    class PMR {
        struct VTable {
            std::allocation_result<void *> (*m_allocateRaw)(void *context, std::size_t bytes, std::align_val_t alignment){};

            void (*m_deallocateRaw)(void *context, void *ptr, std::size_t bytes, std::align_val_t alignment){};

            bool (*m_resizeRaw)(void *context, void *ptr, std::size_t old_size, std::size_t new_size){};
        };

        void *m_context{nullptr};
        const VTable *m_vtable{nullptr};

    public:
        template<details::TAllocator AllocatorT = GPA>
        // ReSharper disable once CppNonExplicitConvertingConstructor
        PMR(AllocatorT = {}) noexcept requires std::is_empty_v<AllocatorT> {
            static constexpr VTable g_vtable{
                .m_allocateRaw = [](void *const, const std::size_t bytes, const std::align_val_t alignment) -> std::allocation_result<void *> {
                    return AllocatorT{}.allocateRaw(bytes, alignment);
                },
                .m_deallocateRaw = [](void *const, void *const ptr, const std::size_t bytes, const std::align_val_t alignment) {
                    AllocatorT{}.deallocateRaw(ptr, bytes, alignment);
                },
                .m_resizeRaw = [](void *, [[maybe_unused]] void *const ptr, [[maybe_unused]] const std::size_t old_size,
                                  [[maybe_unused]] const std::size_t new_size) -> bool {
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
        PMR(AllocatorT &al) noexcept requires (!std::is_empty_v<AllocatorT>)
            : m_context(std::addressof(al)) {
            static constexpr VTable g_vtable{
                .m_allocateRaw = [](void *const context, const std::size_t bytes, const std::align_val_t alignment) -> std::allocation_result<void *> {
                    return static_cast<AllocatorT *>(context)->allocateRaw(bytes, alignment);
                },
                .m_deallocateRaw = [](void *const context, void *const ptr, const std::size_t bytes, const std::align_val_t alignment) {
                    static_cast<AllocatorT *>(context)->deallocateRaw(ptr, bytes, alignment);
                },
                .m_resizeRaw = []([[maybe_unused]] void *const context, [[maybe_unused]] void *const ptr, [[maybe_unused]] const std::size_t old_size,
                                  [[maybe_unused]] const std::size_t new_size) -> bool {
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
        PMR(Allocator<AllocatorT> al) noexcept
            : PMR(al.materialize()) {
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

        [[nodiscard]] friend bool operator==(const PMR &a, const PMR &b) noexcept = default;
    };

    template<>
    struct details::use_inplace<PMR> : std::true_type {
    };

    // ------------------------------------------------------------------
    // pooled 2MiB "huge" pages, serves as backend allocator
    // ------------------------------------------------------------------

    class HugePage {
    public:
        static constexpr std::size_t block_size_v = 2ull << 20u; // 2.0 MiB
        static constexpr std::size_t reserved_size_v = 16ull << 30u; // 16.0 GiB
        static constexpr std::size_t num_reserved_blocks_v = reserved_size_v / block_size_v;

        [[nodiscard]] static PagePool &getGlobalPool() noexcept {
            alignas(hal::cacheline_size_v) static PagePool g_instance{
                block_size_v,
                num_reserved_blocks_v
            };
            return g_instance;
        }

        [[nodiscard]] static auto &getThreadLocalCache() noexcept {
            alignas(hal::cacheline_size_v) thread_local LocalCache<block_size_v, Static<&getGlobalPool> > g_instance_tls{};
            return g_instance_tls;
        }

        [[nodiscard]] PPR_FORCE_INLINE
        static std::allocation_result<void *>
        owns(const std::size_t bytes, const std::align_val_t alignment) {
            return getThreadLocalCache().allocateRaw(bytes, alignment);
        }

        [[nodiscard]] PPR_FORCE_INLINE
        static std::allocation_result<void *>
        allocateRaw(const std::size_t bytes, const std::align_val_t alignment) {
            return getThreadLocalCache().allocateRaw(bytes, alignment);
        }

        PPR_FORCE_INLINE
        static void deallocateRaw(void *const ptr, const std::size_t bytes, const std::align_val_t alignment) {
            getThreadLocalCache().deallocateRaw(ptr, bytes, alignment);
        }
    };

    static_assert(details::use_inplace_v<HugePage>);
    static_assert(details::TBlockAllocator<HugePage>);

    // ------------------------------------------------------------------
    // pooled 32KiB "small" pages, serves as transient allocator
    // ------------------------------------------------------------------

    class SmallPage {
    public:
        // 64.0 or 32.0 KiB to keep aligned with HugePage
        static constexpr std::size_t block_size_v = PPR_32BIT_OR_64BIT(64ull, 32ull) << 10u;

        static constexpr std::size_t reserved_size_v = 64ull << 20u; // 64.0 MiB
        static constexpr std::size_t num_reserved_blocks_v = reserved_size_v / block_size_v;

        struct LocalHint {
            inline static thread_local u32 value{};
        };

        using pooling_allocator_t = HintedPooling<block_size_v, HugePage, num_reserved_blocks_v, LocalHint>;
        static_assert(pooling_allocator_t::pool_size_v == HugePage::block_size_v);

        [[nodiscard]] static pooling_allocator_t &getGlobalPool() noexcept {
            alignas(hal::cacheline_size_v) static pooling_allocator_t g_instance{};
            return g_instance;
        }

        [[nodiscard]] static auto &getThreadLocalCache() noexcept {
            alignas(hal::cacheline_size_v) thread_local
                    LocalCache<block_size_v, Static<&getGlobalPool>, 2u>
                    g_instance_tls{};
            return g_instance_tls;
        }

        [[nodiscard]] PPR_FORCE_INLINE
        static std::allocation_result<void *>
        owns(const std::size_t bytes, const std::align_val_t alignment) {
            return getThreadLocalCache().allocateRaw(bytes, alignment);
        }

        [[nodiscard]] PPR_FORCE_INLINE
        static std::allocation_result<void *>
        allocateRaw(const std::size_t bytes, const std::align_val_t alignment) {
            return getThreadLocalCache().allocateRaw(bytes, alignment);
        }

        PPR_FORCE_INLINE
        static void deallocateRaw(void *const ptr, const std::size_t bytes, const std::align_val_t alignment) {
            getThreadLocalCache().deallocateRaw(ptr, bytes, alignment);
        }
    };



    static_assert(details::use_inplace_v<SmallPage>);
    static_assert(details::TBlockAllocator<SmallPage>);
}
