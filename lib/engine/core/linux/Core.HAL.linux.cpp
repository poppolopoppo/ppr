module;

// For mmap, munmap, mprotect, msync, madvise
#include <sys/mman.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <pwd.h>
#include <cstdlib>
#include <cstring>

// For sysconf
#include <sys/time.h>

// For inotify
#include <sys/inotify.h>
#include <poll.h>

#include "pP/Macros.h"

module engine.core;

import :assert;
import :hal;
import :memory;

import std;

namespace pP::hal {
    [[nodiscard]] std::string_view platformName() noexcept {
        return "linux";
    }

    // ------------------------------------------------------------------
    // operating-system
    // ------------------------------------------------------------------

    [[nodiscard]] std::string_view userName() {
        static const std::string g_username = []() -> std::string {
            // 1. Try $USER environment variable (fast path)
            if (const char *env_user = std::getenv("USER")) {
                if (*env_user != '\0')
                    return std::string(env_user);
            }

            // 2. POSIX fallback: getpwuid
            if (passwd *const pw = ::getpwuid(::getuid())) {
                if (pw->pw_name && *pw->pw_name != '\0')
                    return std::string(pw->pw_name);
            }

            return "unknown_user";
        }();
        return g_username;
    }

    // ------------------------------------------------------------------
    // file-system
    // ------------------------------------------------------------------

    [[nodiscard]] const std::filesystem::directory_entry &homeDir() {
        static const auto g_directory = []() -> std::filesystem::directory_entry {
            if (const char *home = std::getenv("HOME")) {
                return std::filesystem::directory_entry(std::filesystem::path(home));
            }

            // POSIX fallback
            if (passwd *pw = ::getpwuid(::getuid())) {
                return std::filesystem::directory_entry(std::filesystem::path(pw->pw_dir));
            }

            return {};
        }();
        return g_directory;
    }

    [[nodiscard]] const std::filesystem::directory_entry &systemDir() {
        static const std::filesystem::directory_entry g_directory("/usr/bin");
        return g_directory;
    }

    [[nodiscard]] const std::filesystem::directory_entry &appDataLocalDir() {
        static const auto g_directory = []() -> std::filesystem::directory_entry {
            if (const char *xdg = std::getenv("XDG_DATA_HOME")) {
                return std::filesystem::directory_entry(std::filesystem::path(xdg));
            }
            return std::filesystem::directory_entry(
                std::filesystem::path(std::getenv("HOME")) / ".local/share"
            );
        }();
        return g_directory;
    }

    [[nodiscard]] const std::filesystem::directory_entry &appDataRoamingDir() {
        static const auto g_directory = []() -> std::filesystem::directory_entry {
            if (const char *xdg = std::getenv("XDG_CONFIG_HOME")) {
                return std::filesystem::directory_entry(std::filesystem::path(xdg));
            }
            return std::filesystem::directory_entry(
                std::filesystem::path(std::getenv("HOME")) / ".config"
            );
        }();
        return g_directory;
    }

    // ------------------------------------------------------------------
    // memory pages
    // ------------------------------------------------------------------

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

        // If not committing immediately, use madvise to tell the kernel we don't need the pages yet
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

    void pageDecommit(void *const ptr, const std::size_t size) noexcept {
        PPR_ASSERT(ptr != nullptr);
        PPR_ASSERT(std::bit_cast<std::uintptr_t>(ptr) % page_size == 0u);
        PPR_ASSERT(size % page_size == 0u);

        // On Linux, we can use madvise with MADV_DONTNEED to decommit
        // This tells the kernel the pages are no longer needed but keeps the address range
        ::madvise(ptr, size, MADV_DONTNEED);
    }

    void pageProtect(void *const ptr, const std::size_t size, const PageProtection allowed) noexcept(false) {
        const int prot = pageProtectionFlags_(allowed);
        if (::mprotect(ptr, size, prot) != 0) [[unlikely]] {
            throw std::bad_alloc();
        }
    }

    void pageOfferToOS(void *const ptr, const std::size_t size) noexcept {
        // On Linux, use MADV_FREE or MADV_DONTNEED to offer pages to the OS
        // MADV_FREE is available on some Linux versions
#ifdef MADV_FREE
        ::madvise(ptr, size, MADV_FREE);
#else
        ::madvise(ptr, size, MADV_DONTNEED);
#endif
    }

    [[nodiscard]] bool pageReclaimFromOS(const void *const ptr, const std::size_t size) noexcept {
        // On Linux, pages offered with MADV_DONTNEED or MADV_FREE can be reclaimed
        // by accessing them again (they'll be demand-paged back in)
        // Return true to indicate the operation is supported
        (void)ptr;
        (void)size;
        return true;
    }

    void pageFree(void *const ptr, const std::size_t size) noexcept(false) {
        PPR_ASSERT(ptr != nullptr);
#if PPR_ENABLE_ASSERTIONS
        // Verify the pointer is valid using /proc/self/maps or similar
        // For simplicity, just check basic alignment
        PPR_ASSERT(std::bit_cast<std::uintptr_t>(ptr) % page_size == 0u);
        PPR_ASSERT(size % page_size == 0u);
#endif

        if (::munmap(ptr, size) != 0) [[unlikely]] {
            throw std::bad_alloc();
        }
    }

    // ------------------------------------------------------------------
    // native strings
    // ------------------------------------------------------------------

    [[nodiscard]] std::size_t transcode(const std::string_view ansi, char8_t *p_dst, std::size_t capacity) noexcept {
        static_assert(sizeof(char8_t) == sizeof(char));
        const std::size_t n_chars = std::min(ansi.size(), capacity);
        std::memcpy(p_dst, ansi.data(), n_chars * sizeof(char8_t));
        return n_chars;
    }

    [[nodiscard]] std::size_t transcode(const std::string_view ansi, wchar_t *p_dst, std::size_t capacity) noexcept {
        // Linux uses UTF-8 for native strings, but wchar_t is typically UTF-32
        // For simplicity, assume ASCII for ansi->wide conversion
        std::size_t count = 0;
        for (char c : ansi) {
            if (count >= capacity) break;
            p_dst[count++] = static_cast<wchar_t>(static_cast<unsigned char>(c));
        }
        return count;
    }

    [[nodiscard]] std::size_t transcode(const std::u8string_view utf8, wchar_t *p_dst, std::size_t capacity) noexcept {
        // Simplified UTF-8 to UTF-32/wide conversion
        std::size_t count = 0;
        for (auto it = utf8.begin(); it != utf8.end() && count < capacity; ) {
            char32_t cp = static_cast<unsigned char>(*it++);
            if (cp < 0x80) {
                p_dst[count++] = static_cast<wchar_t>(cp);
            } else if ((cp & 0xE0) == 0xC0 && it != utf8.end()) {
                cp = ((cp & 0x1F) << 6) | (*it++ & 0x3F);
                p_dst[count++] = static_cast<wchar_t>(cp);
            } else if ((cp & 0xF0) == 0xE0 && std::distance(it, utf8.end()) >= 2) {
                cp = ((cp & 0x0F) << 12) | ((*it++ & 0x3F) << 6) | (*it++ & 0x3F);
                p_dst[count++] = static_cast<wchar_t>(cp);
            } else if ((cp & 0xF8) == 0xF0 && std::distance(it, utf8.end()) >= 3) {
                cp = ((cp & 0x07) << 18) | ((*it++ & 0x3F) << 12) | ((*it++ & 0x3F) << 6) | (*it++ & 0x3F);
                p_dst[count++] = static_cast<wchar_t>(cp);
            }
        }
        return count;
    }

    [[nodiscard]] std::size_t transcode(const std::wstring_view wide, char8_t *p_dst, std::size_t capacity) noexcept {
        // Simplified wide to UTF-8 conversion
        // Assume wchar_t is UTF-32 on Linux
        std::size_t count = 0;
        for (wchar_t wc : wide) {
            if (count >= capacity) break;
            if (wc < 0x80) {
                p_dst[count++] = static_cast<char8_t>(wc);
            } else if (wc < 0x800) {
                if (count + 1 >= capacity) break;
                p_dst[count++] = static_cast<char8_t>(0xC0 | (wc >> 6));
                p_dst[count++] = static_cast<char8_t>(0x80 | (wc & 0x3F));
            } else {
                if (count + 2 >= capacity) break;
                p_dst[count++] = static_cast<char8_t>(0xE0 | (wc >> 12));
                p_dst[count++] = static_cast<char8_t>(0x80 | ((wc >> 6) & 0x3F));
                p_dst[count++] = static_cast<char8_t>(0x80 | (wc & 0x3F));
            }
        }
        return count;
    }

    [[nodiscard]] std::size_t transcode(const std::wstring_view wide, char *p_dst, std::size_t capacity) noexcept {
        // wchar_t is UTF-32 on Linux; only ASCII-safe chars survive truncation
        std::size_t count = 0;
        for (wchar_t wc : wide) {
            if (count >= capacity) break;
            if (wc >= 0 && wc <= 127)
                p_dst[count++] = static_cast<char>(wc);
            else
                p_dst[count++] = '?';
        }
        return count;
    }

    [[nodiscard]] std::size_t transcode(const std::u8string_view utf8, char *p_dst, std::size_t capacity) noexcept {
        static_assert(sizeof(char8_t) == sizeof(char));
        const std::size_t n = std::min(utf8.size(), capacity);
        std::memcpy(p_dst, utf8.data(), n * sizeof(char));
        return n;
    }

    // ------------------------------------------------------------------
    // debugger
    // ------------------------------------------------------------------

    void outputDebug(const char *ansi_msg) noexcept {
#if PPR_ENABLE_DEBUG
        // Write to stderr
        ::write(STDERR_FILENO, ansi_msg, std::strlen(ansi_msg));
#endif
    }

    void outputDebug(const native::char_t *wide_msg) noexcept {
#if PPR_ENABLE_DEBUG
        // Convert to UTF-8 and output
        std::string converted = toString<char>(native::string_view(wide_msg));
        outputDebug(converted.c_str());
#endif
    }

    [[nodiscard]] bool isDebuggerPresent() noexcept {
#if PPR_ENABLE_DEBUG
        // Check /proc/self/status for TracerPid
        // Simplified check - in production you'd read /proc/self/status
        return false; // Conservative answer
#else
        return false;
#endif
    }

    void breakpoint() noexcept {
#if PPR_ENABLE_DEBUG
        // Use int3 or raise SIGTRAP
        raise(SIGTRAP);
#endif
    }

    void breakpointIfDebugging() noexcept {
#if PPR_ENABLE_DEBUG
        if (isDebuggerPresent()) {
            breakpoint();
        }
#endif
    }

    void breakpointIfDebugging() noexcept {
        // TODO
    }
}

namespace pP::hal::process {
    [[nodiscard]] std::filesystem::path currentExecutablePath() noexcept(false) {
        std::error_code ec;
        auto path = std::filesystem::read_symlink("/proc/self/exe", ec);
        if (ec) {
            throw std::runtime_error("Failed to read /proc/self/exe");
        }
        return path;
    }

    [[nodiscard]] int spawnAndWait(const std::filesystem::path &executable, std::span<const std::string> args) noexcept(false) {
        const pid_t pid = ::fork();
        if (pid == 0) {
            std::vector<const char *> argv;
            argv.reserve(args.size() + 2);
            argv.push_back(executable.c_str());
            for (const auto &arg : args) {
                argv.push_back(arg.c_str());
            }
            argv.push_back(nullptr);

            ::execvp(executable.c_str(), const_cast<char *const *>(argv.data()));
            ::_exit(127);
        }

        if (pid < 0) {
            throw std::runtime_error("fork failed");
        }

        int status = 0;
        ::waitpid(pid, &status, 0);

        if (WIFEXITED(status)) {
            return WEXITSTATUS(status);
        }
        return -1;
    }
}

namespace pP::hal::io {
    // ------------------------------------------------------------------
    // platform-specific state structures (io_uring based)
    // ------------------------------------------------------------------
    //
    // Availability on this platform:
    //   mapFile/unmapFile/mapData/mapSize — fully implemented (mmap).
    //   init/openFile/submit/poll/wait     — throw std::system_error (io_uring not yet wired).
    //

    struct IoHandleData {
        int m_ring_fd{-1};
    };

    struct FileHandleData {
        int m_fd{-1};
    };

    struct MapHandleData {
        void       *m_data{nullptr};
        std::size_t m_size{0};
    };

    // ------------------------------------------------------------------
    // lifecycle
    // ------------------------------------------------------------------

    IoHandle init() noexcept(false) {
        throw std::system_error(
            std::make_error_code(std::errc::operation_not_supported),
            "pP::io: io_uring not yet implemented on Linux");
    }

    void deinit(const IoHandle handle) noexcept {
        delete static_cast<IoHandleData *>(handle);
    }

    // ------------------------------------------------------------------
    // file operations
    // ------------------------------------------------------------------

    FileHandle openFile(const IoHandle, const std::filesystem::path &, const OpenFlags) noexcept(false) {
        throw std::system_error(
            std::make_error_code(std::errc::operation_not_supported),
            "pP::io: openFile not yet implemented on Linux");
    }

    void closeFile(const IoHandle, const FileHandle file) noexcept {
        delete static_cast<FileHandleData *>(file);
    }

    // ------------------------------------------------------------------
    // submit & drain
    // ------------------------------------------------------------------

    std::size_t submit(const IoHandle, const std::span<SubmitEntry>) noexcept {
        return 0u;
    }

    std::size_t poll(const IoHandle, const std::span<CompletionEntry>) noexcept {
        return 0u;
    }

    std::size_t wait(const IoHandle, const std::span<CompletionEntry>) noexcept {
        return 0u;
    }

    void wake(const IoHandle) noexcept {
    }

    // ------------------------------------------------------------------
    // memory-mapped files
    // ------------------------------------------------------------------

    MapHandle mapFile(const std::filesystem::path &path, const OpenFlags flags) noexcept(false) {
        int prot = PROT_READ;
        int oflags = O_RDONLY;

        if (flags.m_bits & OpenFlags::write) {
            prot |= PROT_WRITE;
            oflags = O_RDWR;
        }

        const int fd = ::open(path.c_str(), oflags);
        if (fd < 0) [[unlikely]] {
            throw std::system_error(errno, std::generic_category(), "pP::io: mapFile open failed");
        }

        PPR_DEFER { ::close(fd); };

        struct ::stat st {};
        if (::fstat(fd, &st) < 0) [[unlikely]] {
            throw std::system_error(errno, std::generic_category(), "pP::io: mapFile fstat failed");
        }

        if (st.st_size == 0) {
            auto *md = new MapHandleData();
            return static_cast<MapHandle>(md);
        }

        void *const data = ::mmap(nullptr, static_cast<std::size_t>(st.st_size),
                                  prot, MAP_SHARED, fd, 0);
        if (data == MAP_FAILED) [[unlikely]] {
            throw std::system_error(errno, std::generic_category(), "pP::io: mapFile mmap failed");
        }

        auto *md = new MapHandleData();
        md->m_data = data;
        md->m_size = static_cast<std::size_t>(st.st_size);
        return static_cast<MapHandle>(md);
    }

    void unmapFile(const MapHandle map) noexcept {
        auto *data = static_cast<MapHandleData *>(map);
        if (data != nullptr) {
            if (data->m_data != nullptr) {
                ::munmap(data->m_data, data->m_size);
            }
            delete data;
        }
    }

    void *mapData(const MapHandle map) noexcept {
        const auto *data = static_cast<const MapHandleData *>(map);
        return data != nullptr ? data->m_data : nullptr;
    }

    std::size_t mapSize(const MapHandle map) noexcept {
        const auto *data = static_cast<const MapHandleData *>(map);
        return data != nullptr ? data->m_size : 0u;
    }

    // ------------------------------------------------------------------
    // directory watching
    // ------------------------------------------------------------------

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
        // wd -> relative path from m_root (used for recursive watches)
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

        // Read raw events from inotify fd (non-blocking)
        alignas(::inotify_event) std::byte raw_buf[16384];
        const auto nread = ::read(data->m_inotify_fd, raw_buf, sizeof(raw_buf));
        if (nread < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0u;
            ec = std::error_code(errno, std::generic_category());
            return 0u;
        }
        if (nread == 0) return 0u;

        // Iterate over raw inotify events, write normalized format to buffer.
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

            // Recurse into new directories
            if (data->m_recursive && (ev->mask & IN_CREATE) && (ev->mask & IN_ISDIR)) {
                const auto it = data->m_wd_to_relpath.find(ev->wd);
                if (it != data->m_wd_to_relpath.end()) {
                    const auto sub_rel = it->second / ev->name;
                    const auto sub_abs = data->m_root / sub_rel;
                    addWatchRecursive_(data, sub_abs, sub_rel);
                }
            }

            // Build full relative path
            const auto it = data->m_wd_to_relpath.find(ev->wd);
            std::filesystem::path rel;
            if (it != data->m_wd_to_relpath.end()) {
                rel = it->second / ev->name;
            } else {
                rel = ev->name;
            }
            const auto rel_str = rel.generic_string();
            const u32 path_len = checked_cast<u32>(rel_str.size() + 1u); // +1 for null

            // Check if normalized event fits in caller buffer
            constexpr std::size_t event_header = sizeof(u8) + sizeof(u32);
            if (write_ptr + event_header + path_len > write_end) break;

            write_ptr[0] = static_cast<std::byte>(action);
            write_ptr += sizeof(u8);
            std::memcpy(write_ptr, &path_len, sizeof(u32));
            write_ptr += sizeof(u32);
            std::memcpy(write_ptr, rel_str.data(), path_len - 1u);
            write_ptr[path_len - 1u] = '\0';
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
