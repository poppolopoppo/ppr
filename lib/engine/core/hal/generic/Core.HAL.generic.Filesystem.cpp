module;

#include "pP/Macros.h"

module engine.core;

import :hal;

import std;

namespace pP::hal {
    [[nodiscard]] const std::filesystem::directory_entry &homeDir() {
        static const auto g_directory = []() -> std::filesystem::directory_entry {
            if (const char *home = std::getenv("HOME")) {
                if (*home != '\0')
                    return std::filesystem::directory_entry(home);
            }

            if (const char *profile = std::getenv("USERPROFILE")) {
                if (*profile != '\0')
                    return std::filesystem::directory_entry(profile);
            }

            if (const char *drive = std::getenv("HOMEDRIVE")) {
                if (const char *path = std::getenv("HOMEPATH")) {
                    return std::filesystem::directory_entry(
                        std::filesystem::path(drive) / path
                    );
                }
            }

            return std::filesystem::directory_entry(std::filesystem::current_path());
        }();

        return g_directory;
    }

    [[nodiscard]] const std::filesystem::directory_entry &systemDir() {
        static const auto g_directory = []() -> std::filesystem::directory_entry {
            if (const char *windir = std::getenv("WINDIR")) {
                if (*windir != '\0')
                    return std::filesystem::directory_entry(windir);
            }

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
}
