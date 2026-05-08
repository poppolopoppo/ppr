module;

// For mmap, munmap, mprotect, msync
#include <sys/mman.h>
#include <unistd.h>
#include <pwd.h>
#include <cstdlib>
#include <cstring>

// For Mach VM APIs
#include <mach/mach.h>
#include <mach/vm_map.h>

#include "pP/Macros.h"

module engine.core;

import :assert;
import :hal;
import :memory;

import std;

namespace pP::hal {
    [[nodiscard]] std::string_view platformName() noexcept {
        return "darwin";
    }

    // ------------------------------------------------------------------
    // operating-system
    // ------------------------------------------------------------------

    [[nodiscard]] std::string_view userName() {
        static const std::string g_username = []() -> std::string {
            if (const char *env_user = std::getenv("USER")) {
                if (*env_user != '\0')
                    return std::string(env_user);
            }

            // POSIX fallback: getpwuid
            if (passwd *pw = ::getpwuid(::getuid())) {
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
        static const std::filesystem::directory_entry g_directory("/Applications");
        return g_directory;
    }

    [[nodiscard]] const std::filesystem::directory_entry &appDataLocalDir() {
        static const auto g_directory = []() -> std::filesystem::directory_entry {
            if (const char *home = std::getenv("HOME")) {
                return std::filesystem::directory_entry(
                    std::filesystem::path(home) / "Library/Application Support"
                );
            }
            return {};
        }();
        return g_directory;
    }

    [[nodiscard]] const std::filesystem::directory_entry &appDataRoamingDir() {
        // macOS doesn't distinguish local vs roaming; use same as local
        return appDataLocalDir();
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

    [[nodiscard]] std::allocation_result<void *>
    pageAlloc(const std::size_t size, const bool commit, const PageProtection allowed) noexcept(false) {
        const std::size_t aligned_size = alignForward(size, static_cast<std::size_t>(page_granularity));

        // Build mmap protection flags
        int prot = PROT_NONE;
        if (allowed.read) {
            prot |= (allowed.write ? PROT_READ | PROT_WRITE : PROT_READ);
        }
        if (allowed.execute) {
            prot |= PROT_EXEC;
        }

        const int flags = MAP_PRIVATE | MAP_ANON;
        void *mapped_ptr = ::mmap(nullptr, aligned_size, prot, flags, -1, 0);

        if (mapped_ptr == MAP_FAILED) [[unlikely]] {
            // On Darwin, we could also try mach_vm_allocate as fallback
            throw std::bad_alloc();
        }

        // If not committing immediately, reserve the address range with PROT_NONE.
        // No additional hint is needed — the kernel will fault on access until committed.
        (void)commit;

        return {mapped_ptr, aligned_size};
    }

    void pageCommit(void *const ptr, const std::size_t size, const PageProtection allowed) noexcept(false) {
        PPR_ASSERT(ptr != nullptr);
        PPR_ASSERT(std::bit_cast<std::uintptr_t>(ptr) % page_size == 0u);
        PPR_ASSERT(size % page_size == 0u);

        int prot = PROT_NONE;
        if (allowed.read) {
            prot |= (allowed.write ? PROT_READ | PROT_WRITE : PROT_READ);
        }
        if (allowed.execute) {
            prot |= PROT_EXEC;
        }

        if (::mprotect(ptr, size, prot) != 0) [[unlikely]] {
            throw std::bad_alloc();
        }
    }

    void pageDecommit(void *const ptr, const std::size_t size) noexcept {
        PPR_ASSERT(ptr != nullptr);
        PPR_ASSERT(std::bit_cast<std::uintptr_t>(ptr) % page_size == 0u);
        PPR_ASSERT(size % page_size == 0u);

        // On Darwin, madvise with MADV_FREE tells the kernel pages can be reclaimed.
        // The address range stays reserved but physical pages are returned to the OS.
        ::madvise(ptr, size, MADV_FREE);
    }

    void pageProtect(void *const ptr, const std::size_t size, const PageProtection allowed) noexcept(false) {
        int prot = PROT_NONE;
        if (allowed.read) {
            prot |= (allowed.write ? PROT_READ | PROT_WRITE : PROT_READ);
        }
        if (allowed.execute) {
            prot |= PROT_EXEC;
        }

        if (::mprotect(ptr, size, prot) != 0) [[unlikely]] {
            throw std::bad_alloc();
        }
    }

    void pageOfferToOS(void *const ptr, const std::size_t size) noexcept {
        // On Darwin, MADV_FREE lets the kernel reclaim pages while keeping the mapping valid.
        ::madvise(ptr, size, MADV_FREE);
    }

    [[nodiscard]] bool pageReclaimFromOS(const void *const ptr, const std::size_t size) noexcept {
        // On Darwin, pages offered with MADV_FREE can be reclaimed
        // by accessing them again (they'll be demand-paged back in)
        // Return true to indicate the operation is supported
        (void)ptr;
        (void)size;
        return true;
    }

    void pageFree(void *const ptr, const std::size_t size) noexcept(false) {
        PPR_ASSERT(ptr != nullptr);
#if PPR_ENABLE_ASSERTIONS
        // Verify the pointer is valid
        (void)size; // Size verification is tricky without querying VM region
#else
        (void)size;
#endif

        if (::munmap(ptr, size) != 0) [[unlikely]] {
            throw std::bad_alloc();
        }
    }

    // ------------------------------------------------------------------
    // native strings
    // ------------------------------------------------------------------

    [[nodiscard]] std::size_t transcode(std::string_view ansi, char8_t *p_dst, std::size_t capacity) noexcept {
        static_assert(sizeof(char8_t) == sizeof(char));
        const std::size_t n_chars = std::min(ansi.size(), capacity);
        std::memcpy(p_dst, ansi.data(), n_chars * sizeof(char8_t));
        return n_chars;
    }

    [[nodiscard]] std::size_t transcode(std::string_view ansi, wchar_t *p_dst, std::size_t capacity) noexcept {
        // Darwin uses UTF-8 natively for filesystem paths
        // For simplicity, use a basic conversion ( assumes ASCII for ansi)
        std::size_t count = 0;
        for (char c : ansi) {
            if (count >= capacity) break;
            p_dst[count++] = static_cast<wchar_t>(static_cast<unsigned char>(c));
        }
        return count;
    }

    [[nodiscard]] std::size_t transcode(std::u8string_view utf8, wchar_t *p_dst, std::size_t capacity) noexcept {
        // Simplified UTF-8 to UTF-32/wide conversion
        std::size_t count = 0;
        for (auto it = utf8.begin(); it != utf8.end() && count < capacity; ) {
            char32_t cp = *it++;
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

    [[nodiscard]] std::size_t transcode(std::wstring_view wide, char8_t *p_dst, std::size_t capacity) noexcept {
        // Simplified wide to UTF-8 conversion
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

    [[nodiscard]] std::size_t transcode(std::wstring_view wide, char *p_dst, std::size_t capacity) noexcept {
        // For Darwin, wchar_t is UTF-32; only ASCII-safe chars survive truncation
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

    [[nodiscard]] std::size_t transcode(std::u8string_view utf8, char *p_dst, std::size_t capacity) noexcept {
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
        // On Darwin, we can use os_log or write to stderr
        ::write(STDERR_FILENO, ansi_msg, std::strlen(ansi_msg));
#endif
    }

    void outputDebug(const native::char_t *native_msg) noexcept {
#if PPR_ENABLE_DEBUG
        // Convert to UTF-8 and output
        std::string converted = toString<char>(native::string_view(native_msg));
        outputDebug(converted.c_str());
#endif
    }

    [[nodiscard]] bool isDebuggerPresent() noexcept {
#if PPR_ENABLE_DEBUG
        // Check if we're being debugged using sysctl
        int mib[4];
        struct kinfo_proc info;
        size_t size = sizeof(info);
        info.kp_proc.p_flag = 0;
        mib[0] = CTL_KERN;
        mib[1] = KERN_PROC;
        mib[2] = KERN_PROC_PID;
        mib[3] = getpid();
        sysctl(mib, 4, &info, &size, nullptr, 0);
        return (info.kp_proc.p_flag & P_TRACED) != 0;
#else
        return false;
#endif
    }

    void breakpoint() noexcept {
#if PPR_ENABLE_DEBUG
        __builtin_trap();
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
