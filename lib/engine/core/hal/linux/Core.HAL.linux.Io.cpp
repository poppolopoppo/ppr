module;

#include "pP/Macros.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/io_uring.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cstring>

module engine.core;

import :assert;
import :hal;
import :memory;

import std;

namespace pP::hal::io {
    struct IoHandleData {
        int m_ring_fd{-1};
        int m_eventfd{-1};
        void *m_sq_ptr{nullptr};
        std::size_t m_sq_size{0u};
        void *m_cq_ptr{nullptr};
        std::size_t m_cq_size{0u};
        bool m_single_mmap{false};
        ::io_uring_sqe *m_sqes{nullptr};
        std::size_t m_sqes_size{0u};
        ::io_uring_params m_params{};
        std::mutex m_mutex{};
    };

    struct FileHandleData {
        int m_fd{-1};
        IoHandleData *m_io{nullptr};
    };

    namespace {
        [[nodiscard]] std::uint32_t *sqHead_(IoHandleData *const d) noexcept {
            return static_cast<std::uint32_t *>(
                static_cast<void *>(static_cast<std::byte *>(d->m_sq_ptr) + d->m_params.sq_off.head));
        }

        [[nodiscard]] std::uint32_t *sqTail_(IoHandleData *const d) noexcept {
            return static_cast<std::uint32_t *>(
                static_cast<void *>(static_cast<std::byte *>(d->m_sq_ptr) + d->m_params.sq_off.tail));
        }

        [[nodiscard]] std::uint32_t *sqMask_(IoHandleData *const d) noexcept {
            return static_cast<std::uint32_t *>(
                static_cast<void *>(static_cast<std::byte *>(d->m_sq_ptr) + d->m_params.sq_off.ring_mask));
        }

        [[nodiscard]] std::uint32_t *sqArray_(IoHandleData *const d) noexcept {
            return static_cast<std::uint32_t *>(
                static_cast<void *>(static_cast<std::byte *>(d->m_sq_ptr) + d->m_params.sq_off.array));
        }

        [[nodiscard]] std::uint32_t *cqHead_(IoHandleData *const d) noexcept {
            return static_cast<std::uint32_t *>(
                static_cast<void *>(static_cast<std::byte *>(d->m_cq_ptr) + d->m_params.cq_off.head));
        }

        [[nodiscard]] std::uint32_t *cqTail_(IoHandleData *const d) noexcept {
            return static_cast<std::uint32_t *>(
                static_cast<void *>(static_cast<std::byte *>(d->m_cq_ptr) + d->m_params.cq_off.tail));
        }

        [[nodiscard]] std::uint32_t *cqMask_(IoHandleData *const d) noexcept {
            return static_cast<std::uint32_t *>(
                static_cast<void *>(static_cast<std::byte *>(d->m_cq_ptr) + d->m_params.cq_off.ring_mask));
        }

        [[nodiscard]] ::io_uring_cqe *cqes_(IoHandleData *const d) noexcept {
            return static_cast<::io_uring_cqe *>(
                static_cast<void *>(static_cast<std::byte *>(d->m_cq_ptr) + d->m_params.cq_off.cqes));
        }

        [[nodiscard]] std::uint32_t loadAcquire_(const std::uint32_t *const p) noexcept {
            return __atomic_load_n(p, __ATOMIC_ACQUIRE);
        }

        void storeRelease_(std::uint32_t *const p, const std::uint32_t v) noexcept {
            __atomic_store_n(p, v, __ATOMIC_RELEASE);
        }

        void drainEventfd_(IoHandleData *const d) noexcept {
            if (d->m_eventfd < 0) {
                return;
            }
            std::uint64_t v = 0u;
            while (::read(d->m_eventfd, &v, sizeof(v)) == static_cast<::ssize_t>(sizeof(v))) {
            }
        }

        // Caller holds d->m_mutex. Returns SQE slot or nullptr when the SQ is full.
        [[nodiscard]] ::io_uring_sqe *allocSqeLocked_(IoHandleData *const d) noexcept {
            const std::uint32_t head = loadAcquire_(sqHead_(d));
            const std::uint32_t tail = loadAcquire_(sqTail_(d));
            if (tail - head >= d->m_params.sq_entries) {
                return nullptr;
            }
            const std::uint32_t mask = *sqMask_(d);
            const std::uint32_t idx = tail & mask;
            sqArray_(d)[idx] = idx;
            return &d->m_sqes[idx];
        }

        void commitSqeLocked_(IoHandleData *const d) noexcept {
            const std::uint32_t tail = loadAcquire_(sqTail_(d));
            storeRelease_(sqTail_(d), tail + 1u);
        }

        // Caller holds d->m_mutex. Copies ready CQEs into entries, advances the
        // CQ head, and drains the registered eventfd. No user callback runs here.
        [[nodiscard]] std::size_t drainLocked_(IoHandleData *const d, const std::span<CompletionEntry> entries) noexcept {
            if (d->m_ring_fd < 0 || entries.empty()) {
                return 0u;
            }
            const std::uint32_t head = loadAcquire_(cqHead_(d));
            const std::uint32_t tail = loadAcquire_(cqTail_(d));
            const std::size_t ready = static_cast<std::size_t>(tail - head);
            const std::size_t n = std::min(ready, entries.size());
            if (n == 0u) {
                return 0u;
            }
            const std::uint32_t mask = *cqMask_(d);
            ::io_uring_cqe *const cqes = cqes_(d);
            for (std::size_t i = 0u; i < n; ++i) {
                const ::io_uring_cqe &cqe = cqes[(head + static_cast<std::uint32_t>(i)) & mask];
                CompletionEntry &ce = entries[i];
                ce.m_user_data = std::bit_cast<void *>(cqe.user_data);
                if (cqe.res >= 0) {
                    ce.m_bytes_transferred = static_cast<u64>(static_cast<std::uint32_t>(cqe.res));
                    ce.m_error = {};
                } else {
                    ce.m_bytes_transferred = 0u;
                    ce.m_error = std::error_code(-cqe.res, std::generic_category());
                }
            }
            storeRelease_(cqHead_(d), head + static_cast<std::uint32_t>(n));
            drainEventfd_(d);
            return n;
        }

        void enterSubmit_(IoHandleData *const d, const unsigned to_submit) noexcept {
            if (to_submit == 0u || d->m_ring_fd < 0) {
                return;
            }
            std::ignore = ::syscall(SYS_io_uring_enter, d->m_ring_fd, to_submit, 0u, 0u, nullptr, 0);
        }

        // Stash the request identity where cancelIo can find it. The upper
        // layer passes a per-request overlapped buffer that stays alive while
        // the operation is in flight.
        void stashUserData_(const SubmitEntry &entry) noexcept {
            if (entry.m_overlapped != nullptr) {
                *static_cast<void **>(entry.m_overlapped) = entry.m_user_data;
            }
        }

        [[nodiscard]] void *loadStashedUserData_(void *const overlapped) noexcept {
            if (overlapped == nullptr) {
                return nullptr;
            }
            return *static_cast<void *const *>(overlapped);
        }
    } // namespace

    IoHandle init() noexcept(false) {
        constexpr std::uint32_t kEntries = 256u;

        ::io_uring_params params{};
        const int ring_fd = static_cast<int>(::syscall(SYS_io_uring_setup, kEntries, &params));
        if (ring_fd < 0) [[unlikely]] {
            throw std::system_error(errno, std::generic_category(), "pP::io: io_uring_setup failed");
        }

        auto *data = new IoHandleData();
        data->m_ring_fd = ring_fd;
        data->m_params = params;

        const std::size_t sq_size =
            static_cast<std::size_t>(params.sq_off.array) +
            static_cast<std::size_t>(params.sq_entries) * sizeof(std::uint32_t);
        const std::size_t cq_size =
            static_cast<std::size_t>(params.cq_off.cqes) +
            static_cast<std::size_t>(params.cq_entries) * sizeof(::io_uring_cqe);
        const std::size_t sqes_size =
            static_cast<std::size_t>(params.sq_entries) * sizeof(::io_uring_sqe);

        const bool single = (params.features & IORING_FEAT_SINGLE_MMAP) != 0u;
        data->m_single_mmap = single;

        auto cleanup = [&] noexcept {
            if (data->m_sqes != nullptr) {
                ::munmap(data->m_sqes, data->m_sqes_size);
            }
            if (data->m_sq_ptr != nullptr) {
                ::munmap(data->m_sq_ptr, data->m_sq_size);
            }
            if (data->m_cq_ptr != nullptr && data->m_cq_ptr != data->m_sq_ptr) {
                ::munmap(data->m_cq_ptr, data->m_cq_size);
            }
            ::close(data->m_ring_fd);
            delete data;
        };

        if (single) {
            const std::size_t ring_size = std::max(sq_size, cq_size);
            void *const ptr = ::mmap(nullptr, ring_size, PROT_READ | PROT_WRITE,
                                     MAP_SHARED | MAP_POPULATE, ring_fd, IORING_OFF_SQ_RING);
            if (ptr == MAP_FAILED) [[unlikely]] {
                cleanup();
                throw std::system_error(errno, std::generic_category(), "pP::io: io_uring ring mmap failed");
            }
            data->m_sq_ptr = ptr;
            data->m_cq_ptr = ptr;
            data->m_sq_size = ring_size;
            data->m_cq_size = ring_size;
        } else {
            void *const sq_ptr = ::mmap(nullptr, sq_size, PROT_READ | PROT_WRITE,
                                        MAP_SHARED | MAP_POPULATE, ring_fd, IORING_OFF_SQ_RING);
            if (sq_ptr == MAP_FAILED) [[unlikely]] {
                cleanup();
                throw std::system_error(errno, std::generic_category(), "pP::io: io_uring SQ mmap failed");
            }
            data->m_sq_ptr = sq_ptr;
            data->m_sq_size = sq_size;

            void *const cq_ptr = ::mmap(nullptr, cq_size, PROT_READ | PROT_WRITE,
                                        MAP_SHARED | MAP_POPULATE, ring_fd, IORING_OFF_CQ_RING);
            if (cq_ptr == MAP_FAILED) [[unlikely]] {
                cleanup();
                throw std::system_error(errno, std::generic_category(), "pP::io: io_uring CQ mmap failed");
            }
            data->m_cq_ptr = cq_ptr;
            data->m_cq_size = cq_size;
        }

        void *const sqes = ::mmap(nullptr, sqes_size, PROT_READ | PROT_WRITE,
                                  MAP_SHARED | MAP_POPULATE, ring_fd, IORING_OFF_SQES);
        if (sqes == MAP_FAILED) [[unlikely]] {
            cleanup();
            throw std::system_error(errno, std::generic_category(), "pP::io: io_uring SQE mmap failed");
        }
        data->m_sqes = static_cast<::io_uring_sqe *>(sqes);
        data->m_sqes_size = sqes_size;

        const int efd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (efd >= 0) {
            data->m_eventfd = efd;
            std::ignore = ::syscall(SYS_io_uring_register, ring_fd,
                                    IORING_REGISTER_EVENTFD, &data->m_eventfd, 1);
        }

        return static_cast<IoHandle>(data);
    }

    void deinit(const IoHandle handle) noexcept {
        auto *data = static_cast<IoHandleData *>(handle);
        if (data == nullptr) {
            return;
        }

        // Teardown ordering: prevent new work, wake + drain in flight, then
        // release native resources. Best effort; first error is irrelevant
        // here because the contract is noexcept — never throw from cleanup.
        {
            std::lock_guard lock{data->m_mutex};
            std::array<CompletionEntry, 64> sink{};
            while (drainLocked_(data, sink) != 0u) {
            }
        }

        if (data->m_eventfd >= 0) {
            ::close(data->m_eventfd);
            data->m_eventfd = -1;
        }
        if (data->m_ring_fd >= 0) {
            ::close(data->m_ring_fd);
            data->m_ring_fd = -1;
        }
        if (data->m_sqes != nullptr) {
            ::munmap(data->m_sqes, data->m_sqes_size);
            data->m_sqes = nullptr;
        }
        if (data->m_sq_ptr != nullptr) {
            ::munmap(data->m_sq_ptr, data->m_sq_size);
            data->m_sq_ptr = nullptr;
        }
        if (data->m_cq_ptr != nullptr && (not data->m_single_mmap || data->m_cq_ptr != data->m_sq_ptr)) {
            ::munmap(data->m_cq_ptr, data->m_cq_size);
        }
        data->m_cq_ptr = nullptr;
        delete data;
    }

    FileHandle openFile(const IoHandle io, const std::filesystem::path &path, const OpenFlags flags) noexcept(false) {
        auto *io_data = static_cast<IoHandleData *>(io);
        if (io_data == nullptr) [[unlikely]] {
            throw std::invalid_argument("pP::io: invalid IoHandle");
        }

        int oflags = O_CLOEXEC;
        const bool want_read = (flags.m_bits & OpenFlags::read) != 0u;
        const bool want_write = (flags.m_bits & OpenFlags::write) != 0u;
        if (want_read && want_write) {
            oflags |= O_RDWR;
        } else if (want_write) {
            oflags |= O_RDWR;
        } else {
            oflags |= O_RDONLY;
        }
        if (flags.m_bits & OpenFlags::create) {
            oflags |= O_CREAT;
        }
        if (flags.m_bits & OpenFlags::truncate) {
            oflags |= O_TRUNC;
        }

        const int fd = ::open(path.c_str(), oflags, 0644);
        if (fd < 0) [[unlikely]] {
            throw std::system_error(errno, std::generic_category(), "pP::io: openFile open failed");
        }

        auto *file_data = new FileHandleData();
        file_data->m_fd = fd;
        file_data->m_io = io_data;
        return static_cast<FileHandle>(file_data);
    }

    void closeFile(const IoHandle, const FileHandle file) noexcept {
        auto *data = static_cast<FileHandleData *>(file);
        if (data != nullptr) {
            // Best effort: an in-flight op completes with EBADF and is
            // drained by poll/wait; the request itself is cancelled by its
            // owner before close in normal use.
            if (data->m_fd >= 0) {
                ::close(data->m_fd);
                data->m_fd = -1;
            }
            delete data;
        }
    }

    std::size_t submit(const IoHandle io, const std::span<SubmitEntry> entries) noexcept {
        auto *io_data = static_cast<IoHandleData *>(io);
        if (io_data == nullptr || io_data->m_ring_fd < 0) [[unlikely]] {
            return 0u;
        }

        std::size_t submitted = 0u;
        {
            std::lock_guard lock{io_data->m_mutex};
            for (auto &entry : entries) {
                const auto *file_data = static_cast<const FileHandleData *>(entry.m_file);
                if (file_data == nullptr || file_data->m_fd < 0) {
                    continue;
                }

                PPR_ASSERT(entry.m_overlapped != nullptr);
                ::io_uring_sqe *sqe = allocSqeLocked_(io_data);
                if (sqe == nullptr) {
                    // SQ full: flush queued work so the kernel consumes
                    // entries, then retry once before giving up on this entry.
                    std::ignore = ::syscall(SYS_io_uring_enter, io_data->m_ring_fd,
                                            static_cast<unsigned>(submitted), 0u, 0u, nullptr, 0);
                    submitted = 0u;
                    sqe = allocSqeLocked_(io_data);
                    if (sqe == nullptr) {
                        break;
                    }
                }

                std::memset(sqe, 0, sizeof(*sqe));
                sqe->opcode = entry.m_opcode == Opcode::read ? IORING_OP_READ : IORING_OP_WRITE;
                sqe->fd = file_data->m_fd;
                sqe->addr = std::bit_cast<std::uint64_t>(entry.m_buffer);
                sqe->len = static_cast<std::uint32_t>(entry.m_buffer_size);
                sqe->off = entry.m_file_offset;
                sqe->user_data = std::bit_cast<std::uint64_t>(entry.m_user_data);
                commitSqeLocked_(io_data);

                stashUserData_(entry);
                ++submitted;
            }
        }
        enterSubmit_(io_data, static_cast<unsigned>(submitted));
        return submitted;
    }

    std::size_t poll(const IoHandle io, const std::span<CompletionEntry> entries) noexcept {
        auto *io_data = static_cast<IoHandleData *>(io);
        if (io_data == nullptr) [[unlikely]] {
            return 0u;
        }
        std::lock_guard lock{io_data->m_mutex};
        return drainLocked_(io_data, entries);
    }

    std::size_t wait(const IoHandle io, const std::span<CompletionEntry> entries) noexcept {
        auto *io_data = static_cast<IoHandleData *>(io);
        if (io_data == nullptr || entries.empty()) [[unlikely]] {
            return 0u;
        }
        while (true) {
            {
                std::lock_guard lock{io_data->m_mutex};
                const std::size_t n = drainLocked_(io_data, entries);
                if (n != 0u) {
                    return n;
                }
                if (io_data->m_ring_fd < 0) {
                    return 0u;
                }
            }
            // Block without holding the mutex so wake()/submit() from another
            // thread can post a completion. The kernel returns immediately
            // when a CQE is already pending, so no wakeup is lost.
            const long ret = ::syscall(SYS_io_uring_enter, io_data->m_ring_fd,
                                       0u, 1u, IORING_ENTER_GETEVENTS, nullptr, 0);
            if (ret < 0 && errno != EINTR) {
                std::lock_guard lock{io_data->m_mutex};
                const std::size_t n = drainLocked_(io_data, entries);
                if (n != 0u) {
                    return n;
                }
                return 0u;
            }
        }
    }

    void wake(const IoHandle io) noexcept {
        auto *io_data = static_cast<IoHandleData *>(io);
        if (io_data == nullptr || io_data->m_ring_fd < 0) {
            return;
        }
        unsigned to_submit = 0u;
        {
            std::lock_guard lock{io_data->m_mutex};
            if (::io_uring_sqe *const sqe = allocSqeLocked_(io_data)) {
                std::memset(sqe, 0, sizeof(*sqe));
                sqe->opcode = IORING_OP_NOP;
                sqe->user_data = 0u;
                commitSqeLocked_(io_data);
                to_submit = 1u;
            }
        }
        enterSubmit_(io_data, to_submit);
        if (io_data->m_eventfd >= 0) {
            const std::uint64_t v = 1u;
            std::ignore = ::write(io_data->m_eventfd, &v, sizeof(v));
        }
    }

    void cancelIo(const FileHandle file, void *const overlapped) noexcept {
        const auto *file_data = static_cast<const FileHandleData *>(file);
        if (file_data == nullptr || file_data->m_fd < 0) {
            return;
        }
        IoHandleData *io_data = file_data->m_io;
        if (io_data == nullptr || io_data->m_ring_fd < 0) {
            return;
        }
        void *const target = loadStashedUserData_(overlapped);
        if (target == nullptr) {
            return;
        }
        unsigned to_submit = 0u;
        {
            std::lock_guard lock{io_data->m_mutex};
            if (::io_uring_sqe *const sqe = allocSqeLocked_(io_data)) {
                std::memset(sqe, 0, sizeof(*sqe));
                sqe->opcode = IORING_OP_ASYNC_CANCEL;
                sqe->fd = file_data->m_fd;
                sqe->addr = std::bit_cast<std::uint64_t>(target);
                sqe->user_data = 0u;
                commitSqeLocked_(io_data);
                to_submit = 1u;
            }
        }
        enterSubmit_(io_data, to_submit);
    }
}
