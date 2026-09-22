module;

#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#include "pP/Macros.h"

module engine.core;

import :assert;
import :hal;
import :memory;

import std;

namespace pP::hal {
    void *ringBufferAlloc(const std::size_t buffer_size) noexcept(false) {
        PPR_ASSERT(alignForward(buffer_size, page_granularity) == buffer_size);

        const int fd = static_cast<int>(::syscall(SYS_memfd_create, "pP::ring", static_cast<unsigned>(MFD_CLOEXEC)));
        if (fd < 0) {
            throw std::system_error(errno, std::generic_category(), "pP::hal: memfd_create failed");
        }

        if (::ftruncate(fd, static_cast<off_t>(buffer_size)) != 0) {
            const int create_errno = errno;
            ::close(fd);
            throw std::system_error(create_errno, std::generic_category(), "pP::hal: ftruncate failed");
        }

        // Reserve the 2x window so both views land adjacent without racing other mappings.
        void *const reservation =
            ::mmap(nullptr, 2u * buffer_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (reservation == MAP_FAILED) {
            const int reserve_errno = errno;
            ::close(fd);
            throw std::system_error(reserve_errno, std::generic_category(), "pP::hal: mmap reservation failed");
        }

        void *const view1 =
            ::mmap(reservation, buffer_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
        if (view1 == MAP_FAILED) {
            const int view_errno = errno;
            ::munmap(reservation, 2u * buffer_size);
            ::close(fd);
            throw std::system_error(view_errno, std::generic_category(), "pP::hal: mmap first view failed");
        }

        void *const view2 = ::mmap(static_cast<std::byte *>(reservation) + buffer_size, buffer_size,
            PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
        if (view2 == MAP_FAILED) {
            const int view_errno = errno;
            ::munmap(reservation, 2u * buffer_size);
            ::close(fd);
            throw std::system_error(view_errno, std::generic_category(), "pP::hal: mmap second view failed");
        }

        ::close(fd);
        return view1;
    }

    void ringBufferFree(const void *ring_buffer, const std::size_t buffer_size) noexcept(false) {
        PPR_ASSERT(ring_buffer != nullptr);
        PPR_ASSERT(alignForward(buffer_size, page_granularity) == buffer_size);

        if (::munmap(const_cast<void *>(ring_buffer), 2u * buffer_size) != 0) {
            throw std::system_error(errno, std::generic_category(), "pP::hal: munmap failed");
        }
    }
}
