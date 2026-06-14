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
            "IoPort: io_uring not yet implemented on Linux");
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
            "IoPort: openFile not yet implemented on Linux");
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

    MapHandle mapFile(const IoHandle, const std::filesystem::path &path, const OpenFlags flags) noexcept(false) {
        int prot = PROT_READ;
        int oflags = O_RDONLY;

        if (flags.m_bits & OpenFlags::write) {
            prot |= PROT_WRITE;
            oflags = O_RDWR;
        }

        const int fd = ::open(path.c_str(), oflags);
        if (fd < 0) [[unlikely]] {
            throw std::system_error(errno, std::generic_category(), "IoPort: mapFile open failed");
        }

        PPR_DEFER { ::close(fd); };

        struct ::stat st {};
        if (::fstat(fd, &st) < 0) [[unlikely]] {
            throw std::system_error(errno, std::generic_category(), "IoPort: mapFile fstat failed");
        }

        if (st.st_size == 0) {
            auto *md = new MapHandleData();
            return static_cast<MapHandle>(md);
        }

        void *const data = ::mmap(nullptr, static_cast<std::size_t>(st.st_size),
                                  prot, MAP_SHARED, fd, 0);
        if (data == MAP_FAILED) [[unlikely]] {
            throw std::system_error(errno, std::generic_category(), "IoPort: mapFile mmap failed");
        }

        auto *md = new MapHandleData();
        md->m_data = data;
        md->m_size = static_cast<std::size_t>(st.st_size);
        return static_cast<MapHandle>(md);
    }

    void unmapFile(const IoHandle, const MapHandle map) noexcept {
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
}
