module;

#include "Core.HAL.windows.include.hpp"

// BCryptGenRandom()
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

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

    Uuid Uuid::create() noexcept { // NOLINT(*-pro-type-member-init)
        Uuid result;
        PPR_VERIFY(0 == BCryptGenRandom(
            nullptr,
            reinterpret_cast<PUCHAR>(result.m_data.data()),
            safe_narrowing(result.m_data.size() * sizeof(result.m_data[0])),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG));
        return result;
    }
}
