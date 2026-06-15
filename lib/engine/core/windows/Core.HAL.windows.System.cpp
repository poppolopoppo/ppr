module;

#include "Core.HAL.windows.include.h"

module engine.core;

import :assert;
import :hal;
import :memory;

import std;

namespace pP::hal {
    [[nodiscard]] std::string_view platformName() noexcept {
        return "windows";
    }

    [[nodiscard]] std::string_view userName() {
        static const std::string g_username = []() -> std::string {
            wchar_t buffer[256];
            DWORD size = std::size(buffer);
            if (::GetUserNameW(buffer, &size)) {
                return native::ansi(native::string_view(buffer, size - 1));
            }
            return "unknown_user";
        }();
        return g_username;
    }
}
