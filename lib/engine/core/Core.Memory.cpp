module;
#include <typeinfo>

#include "pP/Macros.h"

module engine.core;
import :memory;
import :memory.pointer;
import std;

namespace pP {
    // ------------------------------------------------------------------
    // general purpose allocator use stl default allocator
    // ------------------------------------------------------------------

    std::allocation_result<void *> mem::GPA::allocateRaw(const std::size_t bytes, const std::align_val_t alignment) noexcept {
        void *const ptr = (alignment > max_align_v ? operator new(bytes, alignment, std::nothrow) : operator new(bytes, std::nothrow));
        PPR_ASSERT(!ptr || alignForward(ptr, alignment) == ptr);
        return {ptr, ptr ? bytes : 0u};
    }

    void mem::GPA::deallocateRaw(void *const ptr, const std::size_t bytes, const std::align_val_t alignment) noexcept {
        if (alignment > max_align_v) {
            operator delete(ptr, bytes, alignment);
        } else {
            operator delete(ptr, bytes);
        }
    }

    // ------------------------------------------------------------------
    // os virtual memory allocator
    // ------------------------------------------------------------------

    std::allocation_result<void *> mem::OS::allocateRaw(const std::size_t bytes, const std::align_val_t alignment) {
        const auto [ptr, reserved] = hal::pageAlloc(bytes);
        PPR_ASSERT(std::bit_cast<std::uintptr_t>(ptr) % static_cast<std::size_t>(alignment) == 0);
        return {ptr, reserved};
    }

    void mem::OS::deallocateRaw(void *const ptr, const std::size_t bytes, [[maybe_unused]] const std::align_val_t alignment) {
        hal::pageFree(ptr, bytes);
    }

    // ------------------------------------------------------------------
    // page pool allocators
    // ------------------------------------------------------------------

    auto mem::HugePage::getGlobalPool() noexcept -> PagePool & {
        alignas(hal::cacheline_size_v) static PagePool g_instance{
            block_size_v,
            num_reserved_blocks_v
        };
        return g_instance;
    }

    auto mem::HugePage::getThreadLocalCache() noexcept -> local_block_cache_t & {
        alignas(hal::cacheline_size_v) thread_local local_block_cache_t g_instance_tls{};
        return g_instance_tls;
    }

    auto mem::SmallPage::getGlobalPool() noexcept -> pooling_allocator_t & {
        alignas(hal::cacheline_size_v) static pooling_allocator_t g_instance{};
        return g_instance;
    }

    auto mem::SmallPage::getThreadLocalCache() noexcept -> local_block_cache_t& {
        alignas(hal::cacheline_size_v) thread_local local_block_cache_t g_instance_tls{};
        return g_instance_tls;
    }
}
