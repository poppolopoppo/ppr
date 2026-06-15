module;

#include "Core.HAL.windows.include.h"

#include <knownfolders.h>
#include <shlobj.h>

module engine.core;

import :assert;
import :hal;
import :memory;

import std;

namespace pP::hal {
    [[nodiscard]] const std::filesystem::directory_entry &homeDir() {
        static const auto g_directory = []() -> std::filesystem::directory_entry {
            wchar_t buffer[MAX_PATH];
            const DWORD len = ::GetEnvironmentVariableW(L"USERPROFILE", buffer, MAX_PATH);
            if (len > 0 && len < MAX_PATH) {
                return std::filesystem::directory_entry(
                    std::filesystem::path(buffer)
                );
            }

            wchar_t drive[MAX_PATH], path[MAX_PATH];
            const DWORD dlen = ::GetEnvironmentVariableW(L"HOMEDRIVE", drive, MAX_PATH);
            const DWORD plen = ::GetEnvironmentVariableW(L"HOMEPATH", path, MAX_PATH);

            if (dlen > 0 && plen > 0) {
                return std::filesystem::directory_entry(
                    std::filesystem::path(std::wstring(drive) + std::wstring(path))
                );
            }

            return {};
        }();
        return g_directory;
    }

    [[nodiscard]] const std::filesystem::directory_entry &systemDir() {
        static const auto g_directory = []() -> std::filesystem::directory_entry {
            wchar_t buffer[MAX_PATH];
            const UINT len = ::GetSystemDirectoryW(buffer, MAX_PATH);

            if (len > 0 && len < MAX_PATH) {
                return std::filesystem::directory_entry(
                    std::filesystem::path(buffer)
                );
            }

            return {};
        }();
        return g_directory;
    }

    [[nodiscard]] const std::filesystem::directory_entry &appDataLocalDir() {
        static const auto g_directory = []() -> std::filesystem::directory_entry {
            PWSTR path = nullptr;

            if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path))) {
                const std::filesystem::path p = path;
                ::CoTaskMemFree(path);
                return std::filesystem::directory_entry(p);
            }

            return {};
        }();
        return g_directory;
    }

    [[nodiscard]] const std::filesystem::directory_entry &appDataRoamingDir() {
        static const auto g_directory = []() -> std::filesystem::directory_entry {
            PWSTR path = nullptr;

            if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &path))) {
                const std::filesystem::path p = path;
                ::CoTaskMemFree(path);
                return std::filesystem::directory_entry(p);
            }

            return {};
        }();
        return g_directory;
    }
}
