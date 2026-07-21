module;

#include "Core.HAL.windows.include.hpp"

module engine.core;

import :assert;
import :hal;
import :memory;

import std;

namespace pP::hal::process {
    [[nodiscard]] std::filesystem::path currentExecutablePath() noexcept(false) {
        wchar_t buffer[MAX_PATH];
        const DWORD len = ::GetModuleFileNameW(nullptr, buffer, MAX_PATH);
        if (len == 0 || len >= MAX_PATH) {
            throw std::runtime_error("Failed to get executable path");
        }
        return std::filesystem::path(buffer, buffer + len);
    }

    [[nodiscard]] int spawnAndWait(const std::filesystem::path &executable, std::span<const std::string> args) noexcept(false) {
        std::wstring cmdline = L"\"" + executable.wstring() + L"\"";
        for (const auto &arg: args) {
            cmdline += L" \"";
            cmdline += toString<wchar_t>(std::string_view(arg));
            cmdline += L'"';
        }

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;

        PROCESS_INFORMATION pi{};

        constexpr DWORD creationFlags = CREATE_NO_WINDOW | CREATE_DEFAULT_ERROR_MODE;

        if (!::CreateProcessW(
            executable.c_str(),
            cmdline.data(),
            nullptr, nullptr, FALSE,
            creationFlags, nullptr, nullptr, &si, &pi)) {
            throw std::runtime_error("Failed to create process");
        }

        ::CloseHandle(pi.hThread);
        ::WaitForSingleObject(pi.hProcess, INFINITE);

        DWORD exit_code = 0;
        if (!::GetExitCodeProcess(pi.hProcess, &exit_code)) {
            exit_code = 0;
        }
        ::CloseHandle(pi.hProcess);

        return static_cast<int>(exit_code);
    }
}
