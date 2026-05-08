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

    [[nodiscard]] std::allocation_result<void *>
    pageAlloc(const std::size_t size, const bool commit, const PageProtection allowed) noexcept(false) {
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
        return utf8.size();
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
}
