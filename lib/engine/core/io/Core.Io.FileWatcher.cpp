module;
#include "pP/Macros.h"

module engine.core;

import :concurrency.event;
import :hal;
import :io.file_watcher;

import std;

namespace pP {

    // ------------------------------------------------------------------
    // construction / destruction
    // ------------------------------------------------------------------

    DirectoryWatcher::DirectoryWatcher(const std::filesystem::path &dir, const bool recursive)
        : m_root(dir)
        , m_recursive(recursive)
    {
        m_handle = hal::io::openWatch(dir, recursive);
    }

    DirectoryWatcher::~DirectoryWatcher() noexcept {
        if (m_handle != nullptr) {
            hal::io::closeWatch(m_handle);
        }
    }

    // ------------------------------------------------------------------
    // move semantics
    // ------------------------------------------------------------------



    // ------------------------------------------------------------------
    // IEvent
    // ------------------------------------------------------------------

    TagPtr<ISignal> DirectoryWatcher::subscribeEvent(const TagPtr<ISignal> signal) noexcept {
        return m_changed.subscribeEvent(signal);
    }

    void DirectoryWatcher::unsubscribeEvent(const TagPtr<ISignal> signal,
                                             const TagPtr<ISignal> restore) noexcept {
        m_changed.unsubscribeEvent(signal, restore);
    }

    bool DirectoryWatcher::pollEvent() noexcept {
        return m_changed.pollEvent();
    }

    void DirectoryWatcher::resetEvent() noexcept {
        m_overflow = false;
        m_error_flag = false;
        m_event_count = 0u;
        m_cache_valid = false;
        m_changed.resetEvent();
    }

    // ------------------------------------------------------------------
    // polling
    // ------------------------------------------------------------------

    void DirectoryWatcher::consume_(const std::size_t bytes) noexcept {
        m_event_count = 0u;
        m_cache_valid = false;

        if (bytes > 0u) {
            m_event_count = hal::io::parseWatchEvents(
                {m_raw, bytes},
                m_events,
                m_names
            );
        }

        const bool has_events = m_event_count > 0u;
        const bool has_error  = static_cast<bool>(m_ec) || m_overflow;

        if (has_events || has_error) {
            m_changed.emitEvent();
        }
    }

    void DirectoryWatcher::poll(std::error_code &ec) noexcept {
        ec = {};
        m_overflow = false;
        m_error_flag = false;
        m_ec = {};

        if (m_handle == nullptr) [[unlikely]] {
            ec = std::make_error_code(std::errc::invalid_argument);
            return;
        }

        const auto bytes = hal::io::pollWatch(m_handle, m_raw, ec);
        m_ec = ec;
        m_overflow = (ec == std::errc::result_out_of_range);
        m_error_flag = static_cast<bool>(ec) && !m_overflow;

        consume_(bytes);
    }

    void DirectoryWatcher::wait(std::error_code &ec) noexcept {
        ec = {};
        m_overflow = false;
        m_error_flag = false;
        m_ec = {};

        if (m_handle == nullptr) [[unlikely]] {
            ec = std::make_error_code(std::errc::invalid_argument);
            return;
        }

        const auto bytes = hal::io::waitWatch(m_handle, m_raw, ec);
        m_ec = ec;
        m_overflow = (ec == std::errc::result_out_of_range);
        m_error_flag = static_cast<bool>(ec) && !m_overflow;

        consume_(bytes);
    }

    std::span<const FileChange> DirectoryWatcher::changes() const noexcept {
        if (m_cache_valid) [[likely]] {
            return {m_cache.data(), m_cache.size()};
        }

        m_cache.clear();
        m_cache.reserve(m_event_count);

        for (std::size_t i = 0u; i < m_event_count; ++i) {
            const auto &ev = m_events[i];
            m_cache.emplace_back(ev.m_action, std::string_view{m_names + ev.m_name_offset});
        }

        m_cache_valid = true;
        return {m_cache.data(), m_cache.size()};
    }

} // namespace pP
