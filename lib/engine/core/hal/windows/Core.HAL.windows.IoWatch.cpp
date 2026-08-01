module;

#include "Core.HAL.windows.include.hpp"

#include <cstddef>

module engine.core;

import :assert;
import :hal;
import :memory;

import std;

namespace pP::hal::io {
    struct WatchHandleData {
        ::HANDLE m_dir{INVALID_HANDLE_VALUE};
        ::HANDLE m_event{nullptr};
        alignas(::DWORD) std::byte m_buffer[65536];
        ::OVERLAPPED m_overlapped{};
        bool m_pending{false};
        bool m_recursive{false};
    };

    static void startWatch_(WatchHandleData *data) noexcept {
        data->m_overlapped = ::OVERLAPPED{};
        data->m_overlapped.hEvent = data->m_event;

        const ::DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME |
                               FILE_NOTIFY_CHANGE_DIR_NAME |
                               FILE_NOTIFY_CHANGE_LAST_WRITE;

        const ::BOOL result = ::ReadDirectoryChangesW(
            data->m_dir,
            data->m_buffer,
            static_cast<::DWORD>(sizeof(data->m_buffer)),
            data->m_recursive ? TRUE : FALSE,
            filter,
            nullptr,
            &data->m_overlapped,
            nullptr);

        data->m_pending = result != FALSE || ::GetLastError() == ERROR_IO_PENDING;
    }

    WatchHandle openWatch(const std::filesystem::path &dir, const bool recursive) noexcept(false) {
        const ::HANDLE hDir = ::CreateFileW(
            dir.c_str(),
            FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
            nullptr);

        if (hDir == INVALID_HANDLE_VALUE) [[unlikely]] {
            throw std::system_error(
                std::error_code(::GetLastError(), std::system_category()));
        }

        auto *data = new WatchHandleData();
        data->m_dir = hDir;
        data->m_recursive = recursive;

        data->m_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (data->m_event == nullptr) [[unlikely]] {
            ::CloseHandle(hDir);
            delete data;
            throw std::system_error(
                std::error_code(::GetLastError(), std::system_category()));
        }

        startWatch_(data);
        return static_cast<WatchHandle>(data);
    }

    void closeWatch(const WatchHandle watch) noexcept {
        auto *data = static_cast<WatchHandleData *>(watch);
        if (data == nullptr) return;

        if (data->m_pending) {
            ::CancelIoEx(data->m_dir, &data->m_overlapped);
            ::WaitForSingleObject(data->m_event, INFINITE);
        }

        if (data->m_event != nullptr) {
            ::CloseHandle(data->m_event);
        }
        if (data->m_dir != INVALID_HANDLE_VALUE) {
            ::CloseHandle(data->m_dir);
        }
        delete data;
    }

    static std::size_t drainWatch_(WatchHandleData *data, const ::DWORD timeout_ms,
                                    const std::span<std::byte> buffer, std::error_code &ec) noexcept {
        if (not data->m_pending) {
            return 0u;
        }

        const ::DWORD wait_result = ::WaitForSingleObject(data->m_event, timeout_ms);
        if (wait_result == WAIT_TIMEOUT) {
            return 0u;
        }
        if (wait_result != WAIT_OBJECT_0) {
            ec = std::error_code(::GetLastError(), std::system_category());
            return 0u;
        }

        ::DWORD bytes_returned = 0u;
        const ::BOOL ok = ::GetOverlappedResult(data->m_dir, &data->m_overlapped, &bytes_returned, FALSE);

        ::ResetEvent(data->m_event);

        data->m_pending = false;

        if (not ok) {
            const ::DWORD err = ::GetLastError();
            ec = std::error_code(err, std::system_category());
            startWatch_(data);
            return 0u;
        }

        if (bytes_returned == 0u) {
            ec = std::make_error_code(std::errc::result_out_of_range);
            startWatch_(data);
            return 0u;
        }

        const auto copy_len = std::min(static_cast<std::size_t>(bytes_returned), buffer.size());
        std::memcpy(buffer.data(), data->m_buffer, copy_len);

        startWatch_(data);
        return copy_len;
    }

    std::size_t pollWatch(const WatchHandle watch, const std::span<std::byte> buffer, std::error_code &ec) noexcept {
        return drainWatch_(static_cast<WatchHandleData *>(watch), 0u, buffer, ec);
    }

    std::size_t waitWatch(const WatchHandle watch, const std::span<std::byte> buffer, std::error_code &ec) noexcept {
        return drainWatch_(static_cast<WatchHandleData *>(watch), INFINITE, buffer, ec);
    }

    std::size_t parseWatchEvents(const std::span<const std::byte> raw,
                                  const std::span<WatchEvent> out_events,
                                  const std::span<char> out_names) noexcept {
        std::size_t event_idx = 0u;
        std::size_t name_offset = 0u;
        // Win32 API writes FILE_NOTIFY_INFORMATION into a DWORD-aligned byte buffer;
        // safe because the struct has standard layout and no stricter alignment.
        const auto *p = reinterpret_cast<const ::FILE_NOTIFY_INFORMATION *>(raw.data());

        while (event_idx < out_events.size()) {
            if (reinterpret_cast<const std::byte *>(p) + offsetof(::FILE_NOTIFY_INFORMATION, FileName) > raw.data() + raw.size()) break;
            if (reinterpret_cast<const std::byte *>(p) + offsetof(::FILE_NOTIFY_INFORMATION, FileName) + p->FileNameLength > raw.data() + raw.size()) break;

            bool known_action = true;
            WatchEvent::Action action{};
            switch (p->Action) {
                case FILE_ACTION_ADDED:            action = WatchEvent::Action::added;    break;
                case FILE_ACTION_REMOVED:          action = WatchEvent::Action::removed;  break;
                case FILE_ACTION_MODIFIED:         action = WatchEvent::Action::modified; break;
                case FILE_ACTION_RENAMED_OLD_NAME: action = WatchEvent::Action::renamed_old; break;
                case FILE_ACTION_RENAMED_NEW_NAME: action = WatchEvent::Action::renamed_new; break;
                default: known_action = false; break;
            }

            if (known_action) {
                out_events[event_idx].m_action = action;

                const int name_len_utf8 = ::WideCharToMultiByte(
                    CP_UTF8, 0,
                    p->FileName, p->FileNameLength / sizeof(WCHAR),
                    out_names.data() + name_offset,
                    checked_cast<int>(out_names.size() - name_offset),
                    nullptr, nullptr);

                if (name_len_utf8 > 0) {
                    out_events[event_idx].m_name_offset = checked_cast<u32>(name_offset);
                    name_offset += static_cast<std::size_t>(name_len_utf8);
                    if (name_offset < out_names.size()) {
                        out_names[name_offset] = '\0';
                        ++name_offset;
                    }
                    ++event_idx;
                }
            }

            if (p->NextEntryOffset == 0u) break;
            p = reinterpret_cast<const ::FILE_NOTIFY_INFORMATION *>(
                reinterpret_cast<const std::byte *>(p) + p->NextEntryOffset);
        }

        return event_idx;
    }
}
