module engine.core;

import :hal;

import std;

#include <unistd.h>
#include <pwd.h>

namespace pP::hal {
    [[nodiscard]] std::string_view platformName() noexcept {
        return "darwin";
    }

    [[nodiscard]] std::string_view userName() {
        static const std::string g_username = []() -> std::string {
            // 1. Try $USER
            if (const char *env_user = std::getenv("USER")) {
                if (*env_user != '\0')
                    return std::string(env_user);
            }

            // 2. POSIX fallback
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
                return std::filesystem::directory_entry(home);
            }

            if (passwd *pw = ::getpwuid(::getuid())) {
                return std::filesystem::directory_entry(pw->pw_dir);
            }

            return {};
        }();
        return g_directory;
    }

    [[nodiscard]] const std::filesystem::directory_entry &systemDir() {
        static const std::filesystem::directory_entry g_directory("/usr/bin");
        return g_directory;
    }

    [[nodiscard]] const std::filesystem::directory_entry &appDataRoamingDir() {
        static const auto g_directory = []() -> std::filesystem::directory_entry {
            const char *home = std::getenv("HOME");
            if (!home)
                return {};

            return std::filesystem::directory_entry(
                std::filesystem::path(home) / "Library" / "Application Support"
            );
        }();
        return g_directory;
    }

    [[nodiscard]] const std::filesystem::directory_entry &appDataLocalDir() {
        static const auto g_directory = []() -> std::filesystem::directory_entry {
            const char *home = std::getenv("HOME");
            if (!home)
                return {};

            return std::filesystem::directory_entry(
                std::filesystem::path(home) / "Library" / "Caches"
            );
        }();
        return g_directory;
    }

    // ------------------------------------------------------------------
    // native strings
    // ------------------------------------------------------------------

    namespace native {
        [[nodiscard]] size_t utf8(const string_view& native_str, char* out_buffer, size_t out_buffer_size) noexcept {
            const size_t n = std::min(native_str.size(), out_buffer_size);
            std::memcpy(out_buffer, native_str.data(), n);
            return n;
        }

        [[nodiscard]] size_t from(const std::string_view& utf8_str, char_t* out_buffer, size_t out_buffer_size) noexcept {
            const size_t n = std::min(utf8_str.size(), out_buffer_size);
            std::memcpy(out_buffer, utf8_str.data(), n);
            return n;
        }
    }

    // ------------------------------------------------------------------
    // debugger
    // ------------------------------------------------------------------

    void outputDebug(const native::char_t *) noexcept {
    }

    [[nodiscard]] bool isDebuggerPresent() noexcept {
        return false;
    }

    void breakpoint() noexcept {
    }

    void breakpointIfDebugging() noexcept {
    }
}
