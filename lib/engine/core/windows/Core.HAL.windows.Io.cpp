module;

#include "Core.HAL.windows.include.h"

#include <Memoryapi.h>

#pragma comment(lib, "mincore.lib")

module engine.core;

import :assert;
import :hal;
import :memory;

import std;

namespace pP::hal::io {
    struct IoHandleData {
        ::HANDLE m_port{nullptr};
    };

    struct FileHandleData {
        ::HANDLE m_file{INVALID_HANDLE_VALUE};
    };

    struct OverlappedExt : public ::OVERLAPPED {
        void *m_user_data{nullptr};
    };

    static_assert(sizeof(OverlappedExt) == sizeof(::OVERLAPPED) + sizeof(void *));
    static_assert(sizeof(OverlappedExt) <= overlapped_storage_size_v);

    static_assert(sizeof(::HANDLE) <= sizeof(::ULONG_PTR),
        "IOCP completion key must be wide enough to hold a HANDLE");

    IoHandle init() noexcept(false) {
        const ::HANDLE port = ::CreateIoCompletionPort(
            INVALID_HANDLE_VALUE, nullptr, 0, 0);
        if (port == nullptr) [[unlikely]] {
            throw std::system_error(
                std::error_code(::GetLastError(), std::system_category()),
                "pP::io: CreateIoCompletionPort failed");
        }

        auto *data = new IoHandleData();
        data->m_port = port;
        return static_cast<IoHandle>(data);
    }

    void deinit(const IoHandle handle) noexcept {
        auto *data = static_cast<IoHandleData *>(handle);
        if (data != nullptr) {
            ::CloseHandle(data->m_port);
            delete data;
        }
    }

    FileHandle openFile(const IoHandle io, const std::filesystem::path &path, const OpenFlags flags) noexcept(false) {
        const auto *io_data = static_cast<const IoHandleData *>(io);
        if (io_data == nullptr) [[unlikely]] {
            throw std::invalid_argument("pP::io: invalid IoHandle");
        }

        ::DWORD access = 0;
        ::DWORD disposition = OPEN_EXISTING;
        ::DWORD share = FILE_SHARE_READ;

        if (flags.m_bits & OpenFlags::read) {
            access |= GENERIC_READ;
        }
        if (flags.m_bits & OpenFlags::write) {
            access |= GENERIC_WRITE;
            share = FILE_SHARE_READ;
        }
        if ((flags.m_bits & (OpenFlags::create | OpenFlags::write)) == (OpenFlags::create | OpenFlags::write)) {
            disposition = OPEN_ALWAYS;
        }
        if (flags.m_bits & OpenFlags::truncate) {
            disposition = CREATE_ALWAYS;
        }

        const ::HANDLE file = ::CreateFileW(
            path.c_str(),
            access,
            share,
            nullptr,
            disposition,
            FILE_FLAG_OVERLAPPED | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr);

        if (file == INVALID_HANDLE_VALUE) [[unlikely]] {
            throw std::system_error(
                std::error_code(::GetLastError(), std::system_category()),
                "pP::io: CreateFileW failed");
        }

        if (::CreateIoCompletionPort(file, io_data->m_port,
                                     std::bit_cast<::ULONG_PTR>(file), 0) == nullptr) [[unlikely]] {
            ::CloseHandle(file);
            throw std::system_error(
                std::error_code(::GetLastError(), std::system_category()),
                "pP::io: CreateIoCompletionPort (assoc) failed");
        }

        ::SetFileCompletionNotificationModes(file, FILE_SKIP_SET_EVENT_ON_HANDLE);

        auto *file_data = new FileHandleData();
        file_data->m_file = file;
        return static_cast<FileHandle>(file_data);
    }

    void closeFile(const IoHandle io, const FileHandle file) noexcept {
        (void)io;
        auto *data = static_cast<FileHandleData *>(file);
        if (data != nullptr) {
            if (data->m_file != INVALID_HANDLE_VALUE) {
                ::CancelIoEx(data->m_file, nullptr);
                ::CloseHandle(data->m_file);
            }
            delete data;
        }
    }

    std::size_t submit(const IoHandle io, const std::span<SubmitEntry> entries) noexcept {
        auto *io_data = static_cast<IoHandleData *>(io);
        if (io_data == nullptr) [[unlikely]] {
            return 0u;
        }

        std::size_t submitted = 0u;
        for (auto &entry : entries) {
            const auto *file_data = static_cast<const FileHandleData *>(entry.m_file);
            if (file_data == nullptr || file_data->m_file == INVALID_HANDLE_VALUE) {
                continue;
            }

            PPR_ASSERT(entry.m_overlapped != nullptr);
            PPR_ASSERT((std::bit_cast<std::uintptr_t>(entry.m_overlapped) % alignof(OverlappedExt)) == 0u);
            auto *overlapped = ::new(entry.m_overlapped) OverlappedExt{};
            overlapped->Offset = static_cast<::DWORD>(entry.m_file_offset & 0xFFFFFFFFu);
            overlapped->OffsetHigh = static_cast<::DWORD>(entry.m_file_offset >> 32u);
            overlapped->m_user_data = entry.m_user_data;

            PPR_ASSERT(entry.m_buffer_size <= static_cast<u64>(std::numeric_limits<::DWORD>::max()));

            ::BOOL result = FALSE;
            if (entry.m_opcode == Opcode::read) {
                result = ::ReadFile(
                    file_data->m_file,
                    entry.m_buffer,
                    static_cast<::DWORD>(entry.m_buffer_size),
                    nullptr,
                    overlapped);
            } else {
                result = ::WriteFile(
                    file_data->m_file,
                    entry.m_buffer,
                    static_cast<::DWORD>(entry.m_buffer_size),
                    nullptr,
                    overlapped);
            }

            if (not result) {
                const ::DWORD err = ::GetLastError();
                if (err != ERROR_IO_PENDING) [[unlikely]] {
                    ::PostQueuedCompletionStatus(
                        io_data->m_port,
                        0,
                        std::bit_cast<::ULONG_PTR>(file_data->m_file),
                        overlapped);
                }
            }
            ++submitted;
        }

        return submitted;
    }

    static std::size_t drainCompletions_(const IoHandle io, const std::span<CompletionEntry> entries,
                                         const ::DWORD timeout_ms) noexcept {
        auto *io_data = static_cast<IoHandleData *>(io);
        if (io_data == nullptr) [[unlikely]] {
            return 0u;
        }

        if (entries.empty()) {
            return 0u;
        }

        ::OVERLAPPED_ENTRY ov_entries[64];
        const ::ULONG max_count = static_cast<::ULONG>(
            std::min(entries.size(), static_cast<std::size_t>(64u)));
        ::ULONG count = 0u;

        const ::BOOL result = ::GetQueuedCompletionStatusEx(
            io_data->m_port,
            ov_entries,
            max_count,
            &count,
            timeout_ms,
            FALSE);

        if (not result) [[unlikely]] {
            return 0u;
        }

        for (::ULONG i = 0u; i < count; ++i) {
            auto *ext = static_cast<OverlappedExt *>(ov_entries[i].lpOverlapped);
            CompletionEntry &ce = entries[static_cast<std::size_t>(i)];
            ce.m_user_data = ext != nullptr ? ext->m_user_data : nullptr;
            ce.m_bytes_transferred = ov_entries[i].dwNumberOfBytesTransferred;

            if (ext != nullptr && ov_entries[i].lpCompletionKey != 0u) {
                const ::HANDLE file_handle = std::bit_cast<::HANDLE>(ov_entries[i].lpCompletionKey);
                ::DWORD dummy;
                if (!::GetOverlappedResult(file_handle, ext, &dummy, FALSE)) {
                    ce.m_error = std::error_code(::GetLastError(), std::system_category());
                }
            } else {
                ce.m_error = {};
            }
        }

        return static_cast<std::size_t>(count);
    }

    std::size_t poll(const IoHandle io, const std::span<CompletionEntry> entries) noexcept {
        return drainCompletions_(io, entries, 0u);
    }

    std::size_t wait(const IoHandle io, const std::span<CompletionEntry> entries) noexcept {
        return drainCompletions_(io, entries, INFINITE);
    }

    void wake(const IoHandle io) noexcept {
        auto *io_data = static_cast<IoHandleData *>(io);
        if (io_data != nullptr) {
            ::PostQueuedCompletionStatus(io_data->m_port, 0, 0, nullptr);
        }
    }

    void cancelIo(const FileHandle file, void *const overlapped) noexcept {
        const auto *data = static_cast<const FileHandleData *>(file);
        if (data != nullptr && data->m_file != INVALID_HANDLE_VALUE) {
            ::CancelIoEx(data->m_file, static_cast<::OVERLAPPED *>(overlapped));
        }
    }
}
