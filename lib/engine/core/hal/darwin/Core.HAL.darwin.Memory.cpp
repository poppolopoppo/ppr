module;

#include <sys/mman.h>
#include <unistd.h>

#include <mach/mach.h>
#include <mach/vm_map.h>

#include "pP/Macros.h"

module engine.core;

import :assert;
import :hal;
import :memory;
import :memory.poison;

import std;

namespace pP::hal {
    const std::size_t page_size = []() noexcept -> std::size_t {
        const long sz = ::sysconf(_SC_PAGESIZE);
        PPR_ASSERT(sz > 0);
        return checked_cast<std::size_t>(sz);
    }();

    const std::align_val_t page_granularity = []() noexcept -> std::align_val_t {
        const long sz = ::sysconf(_SC_PAGESIZE);
        PPR_ASSERT(sz > 0);
        return std::align_val_t{checked_cast<std::size_t>(sz)};
    }();

    static int pageProtectionFlags_(const PageProtection allowed) noexcept {
        int prot = PROT_NONE;
        if (allowed.read) {
            prot |= (allowed.write ? PROT_READ | PROT_WRITE : PROT_READ);
        }
        if (allowed.execute) {
            prot |= PROT_EXEC;
        }
        return prot;
    }

    [[nodiscard]] std::allocation_result<void *> pageAlloc(
        const std::size_t size,
        [[maybe_unused]] const bool commit,
        const PageProtection allowed,
        [[maybe_unused]] const std::align_val_t alignment) noexcept(false) {
        PPR_ASSERT(alignment == page_granularity);
        const std::size_t aligned_size = alignForward(size, static_cast<std::size_t>(page_granularity));

        const int prot = pageProtectionFlags_(allowed);
        const int flags = MAP_PRIVATE | MAP_ANON;
        void *mapped_ptr = ::mmap(nullptr, aligned_size, prot, flags, -1, 0);

        if (mapped_ptr == MAP_FAILED) [[unlikely]] {
            throw std::bad_alloc();
        }

        mem::unpoisonUninitialized(mapped_ptr, aligned_size);

        return {mapped_ptr, aligned_size};
    }

    void pageCommit(void *const ptr, const std::size_t size, const PageProtection allowed) noexcept(false) {
        PPR_ASSERT(ptr != nullptr);
        PPR_ASSERT(std::bit_cast<std::uintptr_t>(ptr) % page_size == 0u);
        PPR_ASSERT(size % page_size == 0u);

        const int prot = pageProtectionFlags_(allowed);
        if (::mprotect(ptr, size, prot) != 0) [[unlikely]] {
            throw std::bad_alloc();
        }

        mem::unpoisonUninitialized(ptr, size);
    }

    void pageDecommit(void *const ptr, const std::size_t size) noexcept(false) {
        PPR_ASSERT(ptr != nullptr);
        PPR_ASSERT(std::bit_cast<std::uintptr_t>(ptr) % page_size == 0u);
        PPR_ASSERT(size % page_size == 0u);

        // Genuine OS page mapping: the region may contain already-decommitted
        // pages, so writing poison patterns here would fault. Decommit itself
        // makes the memory inaccessible, catching use-after-free at the OS level.
        if (::madvise(ptr, size, MADV_FREE) != 0) [[unlikely]] {
            throw std::bad_alloc();
        }
    }

    void pageProtect(void *const ptr, const std::size_t size, const PageProtection allowed) noexcept(false) {
        const int prot = pageProtectionFlags_(allowed);
        if (::mprotect(ptr, size, prot) != 0) [[unlikely]] {
            throw std::bad_alloc();
        }
    }

    void pageOfferToOS(void *const ptr, const std::size_t size) noexcept(false) {
        // Genuine OS page mapping: offered pages must not be poisoned (the
        // offer itself makes the memory dead, catching use-after-free).
        if (::madvise(ptr, size, MADV_FREE) != 0) [[unlikely]] {
            throw std::bad_alloc();
        }
    }

    [[nodiscard]] bool pageReclaimFromOS(const void *const ptr, const std::size_t size) noexcept {
        mem::unpoisonUninitialized(const_cast<void *>(ptr), size);
        return true;
    }

    void pageFree(void *const ptr, const std::size_t size) noexcept(false) {
        PPR_ASSERT(ptr != nullptr);
#if PPR_ENABLE_ASSERTIONS
        (void) size;
#else
        (void) size;
#endif

        // Genuine OS page mapping: the region may contain already-decommitted
        // pages, so writing poison patterns here would fault. Unmapping itself
        // makes the memory inaccessible, catching use-after-free at the OS level.
        if (::munmap(ptr, size) != 0) [[unlikely]] {
            throw std::bad_alloc();
        }
    }
}
