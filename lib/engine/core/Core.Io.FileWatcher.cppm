module;
#include "pP/Macros.h"

export module engine.core:io.file_watcher;

import :assert;
import :concurrency.event;
import :hal;

import std;

export namespace pP {

    using hal::io::WatchEvent;

    struct FileChange {
        WatchEvent::Action m_action;
        std::string_view   m_filename;
    };

    class DirectoryWatcher final : public IEvent {
        static constexpr std::size_t kMaxEvents    = 256u;
        static constexpr std::size_t kNameBufSize  = 16384u;
        static constexpr std::size_t kRawBufSize   = 65536u;

        // --- hot path data (touched every poll / changes call) ---
        hal::io::WatchHandle          m_handle{nullptr};
        std::size_t                   m_event_count{0u};
        std::error_code               m_ec{};
        bool                          m_overflow{false};
        bool                          m_error_flag{false};
        bool                          m_recursive{false};
        mutable bool                  m_cache_valid{false};
        mutable std::vector<FileChange> m_cache{};
        PulseEvent                    m_changed{};
        std::filesystem::path         m_root;

        // --- cold storage ---
        alignas(std::max_align_t) std::byte m_raw[kRawBufSize]{};
        hal::io::WatchEvent                 m_events[kMaxEvents]{};
        char                                m_names[kNameBufSize]{};

        void consume_(const std::size_t bytes) noexcept;

    public:
        DirectoryWatcher() noexcept = default;

        DirectoryWatcher(const std::filesystem::path &dir, bool recursive = false);

        ~DirectoryWatcher() noexcept;

        DirectoryWatcher(const DirectoryWatcher &) = delete;
        DirectoryWatcher &operator=(const DirectoryWatcher &) = delete;
        DirectoryWatcher(DirectoryWatcher &&) = delete;
        DirectoryWatcher &operator=(DirectoryWatcher &&) = delete;

        // --- IEvent ---
        TagPtr<ISignal> subscribeEvent(const TagPtr<ISignal> signal) noexcept override;
        void            unsubscribeEvent(const TagPtr<ISignal> signal,
                                          const TagPtr<ISignal> restore) noexcept override;
        [[nodiscard]] bool pollEvent() noexcept override;
        void               resetEvent() noexcept override;

        // --- Polling (not thread-safe) ---
        void poll(std::error_code &ec) noexcept;
        void wait(std::error_code &ec) noexcept;

        [[nodiscard]] std::span<const FileChange> changes() const noexcept;

        [[nodiscard]] bool                     isOpen() const noexcept { return m_handle != nullptr; }
        [[nodiscard]] const std::filesystem::path &root() const noexcept { return m_root; }
        [[nodiscard]] bool                     hadOverflow() const noexcept { return m_overflow; }
        [[nodiscard]] bool                     hadError() const noexcept { return m_error_flag; }
    };

} // namespace pP
