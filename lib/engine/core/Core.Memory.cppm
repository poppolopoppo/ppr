module;
#include "pP/Macros.h"

export module engine.core:memory;

import :allocator;
import :hal;
import :page_pool;

import std;

export namespace pP::mem {
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

    static_assert(details::use_inplace_v<GPA>);

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

    static_assert(details::use_inplace_v<OS>);

    // ------------------------------------------------------------------
    // pooled 2MiB "huge" pages, serves as backend allocator
    // ------------------------------------------------------------------

    class HugePage {
    public:
        static constexpr std::size_t block_size_v = 2ull << 20u; // 2.0 MiB
        static constexpr std::size_t reserved_size_v = 16ull << 30u; // 16.0 GiB
        static constexpr std::size_t num_reserved_blocks_v = reserved_size_v / block_size_v;

        [[nodiscard]] static os::PagePool &getGlobalPool() noexcept {
            alignas(hal::cacheline_size_v) static os::PagePool g_instance{
                block_size_v,
                num_reserved_blocks_v
            };
            return g_instance;
        }

        using LocalHint = LocalCache<block_size_v, Static<&getGlobalPool> >;

        [[nodiscard]] static LocalHint &getThreadLocalHint() noexcept {
            alignas(hal::cacheline_size_v) thread_local LocalHint g_instance_tls{};
            return g_instance_tls;
        }

        [[nodiscard]] PPR_FORCE_INLINE
        static std::allocation_result<void *>
        owns(const std::size_t bytes, const std::align_val_t alignment) {
            return getThreadLocalHint().allocateRaw(bytes, alignment);
        }

        [[nodiscard]] PPR_FORCE_INLINE
        static std::allocation_result<void *>
        allocateRaw(const std::size_t bytes, const std::align_val_t alignment) {
            return getThreadLocalHint().allocateRaw(bytes, alignment);
        }

        PPR_FORCE_INLINE
        static void deallocateRaw(void *const ptr, const std::size_t bytes, const std::align_val_t alignment) {
            getThreadLocalHint().deallocateRaw(ptr, bytes, alignment);
        }
    };

    static_assert(details::use_inplace_v<HugePage>);

    // ------------------------------------------------------------------
    // pooled 64KiB "small" pages, serves as transient allocator
    // ------------------------------------------------------------------

    class SmallPage {
    public:
        static constexpr std::size_t block_size_v = 64ull << 10u; // 64.0 KiB

        static constexpr std::size_t reserved_size_v = 64ull << 20u; // 64.0 MiB
        static constexpr std::size_t num_reserved_blocks_v = reserved_size_v / block_size_v;

        using pooling_allocator_t = Pooling<block_size_v, HugePage, num_reserved_blocks_v>;

        [[nodiscard]] static pooling_allocator_t &getGlobalPool() noexcept {
            alignas(hal::cacheline_size_v) static pooling_allocator_t g_instance{};
            return g_instance;
        }

        using LocalHint = LocalCache<block_size_v, Static<&getGlobalPool>, 2u>;

        [[nodiscard]] static LocalHint &getThreadLocalHint() noexcept {
            alignas(hal::cacheline_size_v) thread_local LocalHint g_instance_tls{};
            return g_instance_tls;
        }

        [[nodiscard]] PPR_FORCE_INLINE
        static std::allocation_result<void *>
        owns(const std::size_t bytes, const std::align_val_t alignment) {
            return getThreadLocalHint().allocateRaw(bytes, alignment);
        }

        [[nodiscard]] PPR_FORCE_INLINE
        static std::allocation_result<void *>
        allocateRaw(const std::size_t bytes, const std::align_val_t alignment) {
            return getThreadLocalHint().allocateRaw(bytes, alignment);
        }

        PPR_FORCE_INLINE
        static void deallocateRaw(void *const ptr, const std::size_t bytes, const std::align_val_t alignment) {
            getThreadLocalHint().deallocateRaw(ptr, bytes, alignment);
        }
    };

    static_assert(details::use_inplace_v<SmallPage>);
}
