module;

#include <cerrno>
#include <sys/inotify.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>

#include "pP/Macros.h"

module engine.core;

import :assert;
import :hal;
import :memory;

import std;

namespace pP::hal::io {
    namespace {
        ::inotify_event *nextEvent_(::inotify_event *e) noexcept {
            const auto offset = sizeof(::inotify_event) + e->len;
            return reinterpret_cast<::inotify_event *>(
                reinterpret_cast<std::byte *>(e) + offset);
        }
    }

    struct WatchHandleData {
        int m_inotify_fd{-1};
        int m_root_wd{-1};
        std::filesystem::path m_root;
        bool m_recursive{false};
        std::unordered_map<int, std::filesystem::path> m_wd_to_relpath;
    };

    static int addWatchRecursive_(WatchHandleData *data, const std::filesystem::path &dir,
                                   const std::filesystem::path &rel) noexcept {
        const uint32_t mask = IN_CREATE | IN_DELETE | IN_MODIFY |
                              IN_MOVED_FROM | IN_MOVED_TO |
                              IN_ONLYDIR | IN_EXCL_UNLINK;
        const int wd = ::inotify_add_watch(data->m_inotify_fd, dir.c_str(), mask);
        if (wd >= 0) {
            data->m_wd_to_relpath[wd] = rel;
        }
        return wd;
    }

    WatchHandle openWatch(const std::filesystem::path &dir, const bool recursive) noexcept(false) {
        const int fd = ::inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
        if (fd < 0) [[unlikely]] {
            throw std::system_error(errno, std::generic_category(),
                                    "pP::io: openWatch inotify_init1 failed");
        }

        auto *data = new WatchHandleData();
        data->m_inotify_fd = fd;
        data->m_root = dir;
        data->m_recursive = recursive;

        const int wd = addWatchRecursive_(data, dir, std::filesystem::path{});
        if (wd < 0) [[unlikely]] {
            const int saved_errno = errno;
            ::close(fd);
            delete data;
            throw std::system_error(saved_errno, std::generic_category(),
                                    "pP::io: openWatch inotify_add_watch failed");
        }
        data->m_root_wd = wd;

        return static_cast<WatchHandle>(data);
    }

    void closeWatch(const WatchHandle watch) noexcept {
        auto *data = static_cast<WatchHandleData *>(watch);
        if (data == nullptr) return;

        for (const auto &[wd, _] : data->m_wd_to_relpath) {
            ::inotify_rm_watch(data->m_inotify_fd, wd);
        }
        ::close(data->m_inotify_fd);
        delete data;
    }

    std::size_t pollWatch(const WatchHandle watch, const std::span<std::byte> buffer, std::error_code &ec) noexcept {
        auto *data = static_cast<WatchHandleData *>(watch);
        if (data == nullptr) return 0u;

        alignas(::inotify_event) std::byte raw_buf[16384];
        const auto nread = ::read(data->m_inotify_fd, raw_buf, sizeof(raw_buf));
        if (nread < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0u;
            ec = std::error_code(errno, std::generic_category());
            return 0u;
        }
        if (nread == 0) return 0u;

        auto *write_ptr = buffer.data();
        const auto *write_end = buffer.data() + buffer.size();
        auto *raw_ptr = raw_buf;
        const auto *raw_end = raw_buf + static_cast<std::size_t>(nread);

        while (raw_ptr + static_cast<std::ptrdiff_t>(sizeof(::inotify_event)) <= raw_end) {
            const auto *ev = reinterpret_cast<const ::inotify_event *>(raw_ptr);
            if (raw_ptr + static_cast<std::ptrdiff_t>(sizeof(::inotify_event) + ev->len) > raw_end) break;

            if (ev->mask & IN_Q_OVERFLOW) {
                ec = std::make_error_code(std::errc::result_out_of_range);
                raw_ptr = reinterpret_cast<std::byte *>(nextEvent_(const_cast<::inotify_event *>(ev)));
                continue;
            }

            WatchEvent::Action action{};
            if (ev->mask & IN_CREATE)     action = WatchEvent::Action::added;
            else if (ev->mask & IN_DELETE) action = WatchEvent::Action::removed;
            else if (ev->mask & IN_MODIFY) action = WatchEvent::Action::modified;
            else if (ev->mask & IN_MOVED_FROM) action = WatchEvent::Action::renamed_old;
            else if (ev->mask & IN_MOVED_TO)   action = WatchEvent::Action::renamed_new;
            else {
                raw_ptr = reinterpret_cast<std::byte *>(nextEvent_(const_cast<::inotify_event *>(ev)));
                continue;
            }

            if (data->m_recursive && (ev->mask & IN_CREATE) && (ev->mask & IN_ISDIR)) {
                const auto it = data->m_wd_to_relpath.find(ev->wd);
                if (it != data->m_wd_to_relpath.end()) {
                    const auto sub_rel = it->second / ev->name;
                    const auto sub_abs = data->m_root / sub_rel;
                    addWatchRecursive_(data, sub_abs, sub_rel);
                }
            }

            const auto it = data->m_wd_to_relpath.find(ev->wd);
            std::filesystem::path rel;
            if (it != data->m_wd_to_relpath.end()) {
                rel = it->second / ev->name;
            } else {
                rel = ev->name;
            }
            const auto rel_str = rel.generic_string();
            const u32 path_len = checked_cast<u32>(rel_str.size() + 1u);

            constexpr std::size_t event_header = sizeof(u8) + sizeof(u32);
            if (write_ptr + event_header + path_len > write_end) break;

            write_ptr[0] = static_cast<std::byte>(action);
            write_ptr += sizeof(u8);
            std::memcpy(write_ptr, &path_len, sizeof(u32));
            write_ptr += sizeof(u32);
            std::memcpy(write_ptr, rel_str.data(), path_len - 1u);
            write_ptr[path_len - 1u] = std::byte{0};
            write_ptr += path_len;

            raw_ptr = reinterpret_cast<std::byte *>(nextEvent_(const_cast<::inotify_event *>(ev)));
        }

        return static_cast<std::size_t>(write_ptr - buffer.data());
    }

    std::size_t waitWatch(const WatchHandle watch, const std::span<std::byte> buffer, std::error_code &ec) noexcept {
        auto *data = static_cast<WatchHandleData *>(watch);
        if (data == nullptr) return 0u;

        struct ::pollfd pfd{};
        pfd.fd = data->m_inotify_fd;
        pfd.events = POLLIN;

        const int ret = ::poll(&pfd, 1, -1);
        if (ret < 0) {
            ec = std::error_code(errno, std::generic_category());
            return 0u;
        }

        return pollWatch(watch, buffer, ec);
    }

    std::size_t parseWatchEvents(const std::span<const std::byte> raw,
                                  const std::span<WatchEvent> out_events,
                                  const std::span<char> out_names) noexcept {
        std::size_t event_idx = 0u;
        std::size_t name_offset = 0u;
        auto *p = raw.data();
        const auto *const end = p + raw.size();

        while (p + static_cast<std::ptrdiff_t>(sizeof(u8) + sizeof(u32)) <= end && event_idx < out_events.size()) {
            const auto action = static_cast<WatchEvent::Action>(*p); p += sizeof(u8);
            u32 name_len = 0u;
            std::memcpy(&name_len, p, sizeof(u32)); p += sizeof(u32);
            if (p + static_cast<std::ptrdiff_t>(name_len) > end) break;
            if (name_offset + name_len > out_names.size()) break;

            out_events[event_idx].m_action = action;
            out_events[event_idx].m_name_offset = checked_cast<u32>(name_offset);
            std::memcpy(out_names.data() + name_offset, p, name_len);
            out_names[name_offset + name_len - 1u] = '\0';

            ++event_idx;
            name_offset += name_len;
            p += name_len;
        }

        return event_idx;
    }
}
