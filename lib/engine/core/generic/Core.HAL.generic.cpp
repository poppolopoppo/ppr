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
