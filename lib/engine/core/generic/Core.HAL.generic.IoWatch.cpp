module;

#include "pP/Macros.h"

module engine.core;

import :assert;
import :hal;
import :memory;

import std;

namespace pP::hal::io {
    struct WatchHandleData {
        std::filesystem::path m_path;
        bool m_recursive{false};
        std::unordered_map<std::string, std::filesystem::file_time_type> m_snapshot;
    };

    static void buildSnapshot_(const std::filesystem::path &dir, bool recursive,
                               std::unordered_map<std::string, std::filesystem::file_time_type> &snap) noexcept {
        snap.clear();
        auto iter = recursive
            ? std::filesystem::recursive_directory_iterator(dir,
                  std::filesystem::directory_options::skip_permission_denied)
            : std::filesystem::directory_iterator(dir,
                  std::filesystem::directory_options::skip_permission_denied);
        for (auto &entry : iter) {
            std::error_code ec;
            const auto ft = entry.last_write_time(ec);
            if (not ec) {
                snap[entry.path().lexically_relative(dir).generic_string()] = ft;
            }
        }
    }

    WatchHandle openWatch(const std::filesystem::path &dir, const bool recursive) noexcept(false) {
        auto *data = new WatchHandleData();
        data->m_path = dir;
        data->m_recursive = recursive;
        buildSnapshot_(dir, recursive, data->m_snapshot);
        return static_cast<WatchHandle>(data);
    }

    void closeWatch(const WatchHandle watch) noexcept {
        delete static_cast<WatchHandleData *>(watch);
    }

    std::size_t pollWatch(const WatchHandle watch, const std::span<std::byte> buffer, std::error_code &ec) noexcept {
        auto *data = static_cast<WatchHandleData *>(watch);
        if (data == nullptr) {
            ec = std::make_error_code(std::errc::invalid_argument);
            return 0u;
        }

        std::unordered_map<std::string, std::filesystem::file_time_type> current;
        buildSnapshot_(data->m_path, data->m_recursive, current);

        auto writeEvent = [&](const WatchEvent::Action action, const std::string &name) -> bool {
            const auto name_len = name.size() + 1u;
            const auto total = sizeof(u8) + sizeof(u32) + name_len;
            if (total > buffer.size()) {
                return false;
            }
            auto *p = buffer.data();
            *p = static_cast<u8>(action); p += sizeof(u8);
            const u32 nl = checked_cast<u32>(name_len);
            std::memcpy(p, &nl, sizeof(u32)); p += sizeof(u32);
            std::memcpy(p, name.c_str(), name_len);
            return true;
        };

        std::size_t written = 0u;

        for (auto &[name, ft] : data->m_snapshot) {
            if (not current.contains(name)) {
                const auto removed_size = sizeof(u8) + sizeof(u32) + name.size() + 1u;
                if (not writeEvent(WatchEvent::Action::removed, name)) break;
                buffer = buffer.subspan(removed_size);
                written += removed_size;
            }
        }

        for (auto &[name, ft] : current) {
            if (auto it = data->m_snapshot.find(name); it == data->m_snapshot.end()) {
                const auto added_size = sizeof(u8) + sizeof(u32) + name.size() + 1u;
                if (not writeEvent(WatchEvent::Action::added, name)) break;
                buffer = buffer.subspan(added_size);
                written += added_size;
            } else if (it->second != ft) {
                const auto mod_size = sizeof(u8) + sizeof(u32) + name.size() + 1u;
                if (not writeEvent(WatchEvent::Action::modified, name)) break;
                buffer = buffer.subspan(mod_size);
                written += mod_size;
            }
        }

        data->m_snapshot = std::move(current);
        return written;
    }

    std::size_t waitWatch(const WatchHandle watch, const std::span<std::byte> buffer, std::error_code &ec) noexcept {
        for (;;) {
            const auto n = pollWatch(watch, buffer, ec);
            if (n > 0 || ec) return n;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    std::size_t parseWatchEvents(const std::span<const std::byte> raw,
                                  const std::span<WatchEvent> out_events,
                                  const std::span<char> out_names) noexcept {
        std::size_t event_idx = 0u;
        std::size_t name_offset = 0u;
        auto *p = raw.data();
        const auto *const end = p + raw.size();

        while (p + sizeof(u8) + sizeof(u32) <= end && event_idx < out_events.size()) {
            const auto action = static_cast<WatchEvent::Action>(*p); p += sizeof(u8);
            u32 name_len = 0u;
            std::memcpy(&name_len, p, sizeof(u32)); p += sizeof(u32);
            if (p + name_len > end) break;
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
