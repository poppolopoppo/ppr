module;

#include "pP/Macros.h"

module engine.core;

import :hal;

import std;

namespace pP::hal {
    [[nodiscard]] std::string_view platformName() noexcept {
        return "generic";
    }

    [[nodiscard]] std::string_view userName() {
        static const std::string g_username = []() -> std::string {
            // Try common environment variables
            if (const char *user = std::getenv("USER")) {
                if (*user != '\0')
                    return std::string(user);
            }

            if (const char *user = std::getenv("USERNAME")) {
                if (*user != '\0')
                    return std::string(user);
            }

            // Last‑resort fallback
            return "unknown_user";
        }();

        return g_username;
    }

    // ------------------------------------------------------------------
    // file-system
    // ------------------------------------------------------------------

    [[nodiscard]] const std::filesystem::directory_entry &homeDir() {
        static const auto g_directory = []() -> std::filesystem::directory_entry {
            // Universal on Unix-like systems and many Windows shells
            if (const char *home = std::getenv("HOME")) {
                if (*home != '\0')
                    return std::filesystem::directory_entry(home);
            }

            // Windows fallback
            if (const char *profile = std::getenv("USERPROFILE")) {
                if (*profile != '\0')
                    return std::filesystem::directory_entry(profile);
            }

            // Windows split fallback
            if (const char *drive = std::getenv("HOMEDRIVE")) {
                if (const char *path = std::getenv("HOMEPATH")) {
                    return std::filesystem::directory_entry(
                        std::filesystem::path(drive) / path
                    );
                }
            }

            // Last‑resort fallback: current directory
            return std::filesystem::directory_entry(std::filesystem::current_path());
        }();

        return g_directory;
    }

    [[nodiscard]] const std::filesystem::directory_entry &systemDir() {
        static const auto g_directory = []() -> std::filesystem::directory_entry {
            // Try environment variable commonly used on Windows
            if (const char *windir = std::getenv("WINDIR")) {
                if (*windir != '\0')
                    return std::filesystem::directory_entry(windir);
            }

            // Try root directory (exists everywhere)
            return std::filesystem::directory_entry(std::filesystem::path("/"));
        }();

        return g_directory;
    }


    [[nodiscard]] const std::filesystem::directory_entry &appDataLocalDir() {
        return homeDir();
    }

    [[nodiscard]] const std::filesystem::directory_entry &appDataRoamingDir() {
        return homeDir();
    }

    // ------------------------------------------------------------------
    // native strings
    // ------------------------------------------------------------------
    const std::size_t page_size = 4096u;
    const std::align_val_t page_granularity{4096u};

    [[nodiscard]] std::allocation_result<void *> pageAlloc(
        const std::size_t size,
        const bool commit,
        const PageProtection allowed,
        std::align_val_t alignment) noexcept(false) {
        (void)size;
        (void)commit;
        (void)allowed;
        (void)alignment;
        throw std::bad_alloc();
    }

    void pageCommit(void *const ptr, const std::size_t size, const PageProtection allowed) noexcept(false) {
        (void)ptr;
        (void)size;
        (void)allowed;
        throw std::bad_alloc();
    }

    void pageDecommit(void *const ptr, const std::size_t size) noexcept(false) {
        (void)ptr;
        (void)size;
    }

    void pageProtect(void *const ptr, const std::size_t size, const PageProtection allowed) noexcept(false) {
        (void)ptr;
        (void)size;
        (void)allowed;
    }

    void pageOfferToOS(void *const ptr, const std::size_t size) noexcept {
        (void)ptr;
        (void)size;
    }

    [[nodiscard]] bool pageReclaimFromOS(const void *const ptr, const std::size_t size) noexcept {
        (void)ptr;
        (void)size;
        return false;
    }

    void pageFree(void *const ptr, const std::size_t size) noexcept(false) {
        (void)ptr;
        (void)size;
    }

    [[nodiscard]] std::size_t transcode(const std::string_view ansi, char8_t *p_dst, const std::size_t capacity) noexcept {
        const std::size_t n = std::min(ansi.size(), capacity);
        std::memcpy(p_dst, ansi.data(), n * sizeof(char8_t));
        return n;
    }

    [[nodiscard]] std::size_t transcode(const std::string_view ansi, wchar_t *p_dst, const std::size_t capacity) noexcept {
        const std::size_t n = std::min(ansi.size(), capacity);
        for (std::size_t i = 0; i < n; ++i) {
            p_dst[i] = static_cast<wchar_t>(static_cast<unsigned char>(ansi[i]));
        }
        return n;
    }

    [[nodiscard]] std::size_t transcode(const std::u8string_view utf8, wchar_t *p_dst, const std::size_t capacity) noexcept {
        const std::size_t n = std::min(utf8.size(), capacity);
        for (std::size_t i = 0; i < n; ++i) {
            p_dst[i] = static_cast<wchar_t>(utf8[i]);
        }
        return n;
    }

    [[nodiscard]] std::size_t transcode(const std::wstring_view wide, char8_t *p_dst, const std::size_t capacity) noexcept {
        const std::size_t n = std::min(wide.size(), capacity);
        for (std::size_t i = 0; i < n; ++i) {
            p_dst[i] = static_cast<char8_t>(wide[i] & 0xFF);
        }
        return n;
    }

    [[nodiscard]] std::size_t transcode(const std::wstring_view wide, char *const p_dst, const std::size_t capacity) noexcept {
        const std::size_t n = std::min(wide.size(), capacity);
        for (std::size_t i = 0; i < n; ++i) {
            p_dst[i] = static_cast<char>(wide[i] & 0xFF);
        }
        return n;
    }

    [[nodiscard]] std::size_t transcode(const std::u8string_view utf8, char *const p_dst, const std::size_t capacity) noexcept {
        const std::size_t n = std::min(utf8.size(), capacity);
        std::memcpy(p_dst, utf8.data(), n * sizeof(char));
        return n;
    }

    void outputDebug(const char *ansi_msg) noexcept {
        (void)ansi_msg;
    }

    void outputDebug(const native::char_t *native_msg) noexcept {
        (void)native_msg;
    }

    [[nodiscard]] bool isDebuggerPresent() noexcept {
        return false;
    }

    void breakpoint() noexcept {
    }

    void breakpointIfDebugging() noexcept {
    }
}

namespace pP::hal::io {
    // ------------------------------------------------------------------
    // asynchronous I/O
    // ------------------------------------------------------------------
    // Stub: no async I/O primitives available on this platform.
    // All operations throw std::system_error(operation_not_supported).
    // ------------------------------------------------------------------

    struct IoHandleData { int dummy{}; };
    struct FileHandleData { int dummy{}; };
    struct MapHandleData { void *m_data{}; std::size_t m_size{}; };

    IoHandle init() noexcept(false) {
        throw std::system_error(
            std::make_error_code(std::errc::operation_not_supported),
            "IoPort not supported on this platform");
    }

    void deinit(const IoHandle) noexcept {}

    FileHandle openFile(const IoHandle, const std::filesystem::path &, const OpenFlags) noexcept(false) {
        throw std::system_error(
            std::make_error_code(std::errc::operation_not_supported),
            "IoPort not supported on this platform");
    }

    void closeFile(const IoHandle, const FileHandle) noexcept {}

    std::size_t submit(const IoHandle, const std::span<SubmitEntry>) noexcept { return 0u; }
    std::size_t poll(const IoHandle, const std::span<CompletionEntry>) noexcept { return 0u; }
    std::size_t wait(const IoHandle, const std::span<CompletionEntry>) noexcept { return 0u; }
    void wake(const IoHandle) noexcept {}

    MapHandle mapFile(const std::filesystem::path &, const OpenFlags) noexcept(false) {
        throw std::system_error(
            std::make_error_code(std::errc::operation_not_supported),
            "pP::io: memory-mapped files not supported on this platform");
    }

    void unmapFile(const MapHandle) noexcept {}

    void *mapData(const MapHandle) noexcept { return nullptr; }
    std::size_t mapSize(const MapHandle) noexcept { return 0u; }
}

namespace pP::hal::process {
    [[nodiscard]] std::filesystem::path currentExecutablePath() noexcept(false) {
        throw std::runtime_error("currentExecutablePath not implemented for generic platform");
    }

    [[nodiscard]] int spawnAndWait(const std::filesystem::path &executable, std::span<const std::string> args) noexcept(false) {
        (void)executable;
        (void)args;
        throw std::runtime_error("spawnAndWait not implemented for generic platform");
    }
}
