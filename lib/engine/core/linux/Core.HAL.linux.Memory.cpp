module;

#include <sys/mman.h>
#include <unistd.h>

#include <sys/time.h>

#include <linux/memfd.h>
#include <cstdlib>
#include <cstring>

#include "pP/Macros.h"

module engine.core;

import :assert;
import :hal;
import :memory;

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

    static int pageProtectionFlags_(const PageProtection protect) noexcept {
        int prot = PROT_NONE;
        if (protect.read) {
            prot |= (protect.write ? PROT_READ | PROT_WRITE : PROT_READ);
        }
        if (protect.execute) {
            prot |= PROT_EXEC;
        }
        return prot;
    }

    [[nodiscard]] std::allocation_result<void *> pageAlloc(
        const std::size_t size,
        const bool commit,
        const PageProtection allowed,
        [[maybe_unused]] std::align_val_t alignment) noexcept(false) {
        PPR_ASSERT(alignment == page_granularity);
        const std::size_t aligned_size = alignForward(size, static_cast<std::size_t>(page_granularity));

        const int prot = pageProtectionFlags_(allowed);
        const int flags = MAP_PRIVATE | MAP_ANONYMOUS;

        void *mapped_ptr = ::mmap(nullptr, aligned_size, prot, flags, -1, 0);

        if (mapped_ptr == MAP_FAILED) [[unlikely]] {
            throw std::bad_alloc();
        }

        if (!commit) {
            ::madvise(mapped_ptr, aligned_size, MADV_DONTNEED);
        }

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
    }

    void pageDecommit(void *const ptr, const std::size_t size) noexcept(false) {
        PPR_ASSERT(ptr != nullptr);
        PPR_ASSERT(std::bit_cast<std::uintptr_t>(ptr) % page_size == 0u);
        PPR_ASSERT(size % page_size == 0u);

        if (::madvise(ptr, size, MADV_DONTNEED) != 0) [[unlikely]] {
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
#ifdef MADV_FREE
        if (::madvise(ptr, size, MADV_FREE) != 0) [[unlikely]]
#else
        if (::madvise(ptr, size, MADV_DONTNEED) != 0) [[unlikely]]
#endif
        {
            throw std::bad_alloc();
        }
    }

    [[nodiscard]] bool pageReclaimFromOS(const void *const ptr, const std::size_t size) noexcept {
        (void)ptr;
        (void)size;
        return true;
    }

    void pageFree(void *const ptr, const std::size_t size) noexcept(false) {
        PPR_ASSERT(ptr != nullptr);
#if PPR_ENABLE_ASSERTIONS
        PPR_ASSERT(std::bit_cast<std::uintptr_t>(ptr) % page_size == 0u);
        PPR_ASSERT(size % page_size == 0u);
#endif

        if (::munmap(ptr, size) != 0) [[unlikely]] {
            throw std::bad_alloc();
        }
    }

    void *ringBufferAlloc(const std::size_t buffer_size) noexcept(false) {
        const std::size_t total_size = buffer_size * 2;

        void *storage = nullptr;
        if (::posix_memalign(&storage, 4096, total_size) != 0) {
            throw std::bad_alloc();
        }
        std::memset(storage, 0, total_size);

        int fd = ::memfd_create("ringbuf", MFD_CLOEXEC);
        if (fd < 0) {
            std::free(storage);
            throw std::bad_alloc();
        }

        if (::ftruncate(fd, static_cast<off_t>(buffer_size)) < 0) {
            ::close(fd);
            std::free(storage);
            throw std::bad_alloc();
        }

        void *mapping1 = ::mmap(storage, buffer_size, PROT_READ | PROT_WRITE,
                                MAP_FIXED | MAP_SHARED, fd, 0);
        if (mapping1 == MAP_FAILED) {
            ::close(fd);
            std::free(storage);
            throw std::bad_alloc();
        }

        void *mapping2 = ::mmap(static_cast<u8 *>(storage) + buffer_size, buffer_size,
                                PROT_READ | PROT_WRITE, MAP_FIXED | MAP_SHARED, fd, 0);
        ::close(fd);

        if (mapping2 == MAP_FAILED) {
            ::munmap(storage, buffer_size);
            throw std::bad_alloc();
        }

        return storage;
    }

    void ringBufferFree(const void *ring_buffer, const std::size_t buffer_size) noexcept(false) {
        if (!ring_buffer) return;
        ::munmap(const_cast<void *>(ring_buffer), buffer_size * 2);
    }
}
