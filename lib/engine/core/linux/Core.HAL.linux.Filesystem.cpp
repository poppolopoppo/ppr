module;

#include <pwd.h>
#include <unistd.h>

#include "pP/Macros.h"

module engine.core;

import :assert;
import :hal;
import :memory;

import std;

namespace pP::hal {
    [[nodiscard]] const std::filesystem::directory_entry &homeDir() {
        static const auto g_directory = []() -> std::filesystem::directory_entry {
            if (const char *home = std::getenv("HOME")) {
                return std::filesystem::directory_entry(std::filesystem::path(home));
            }

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
}
