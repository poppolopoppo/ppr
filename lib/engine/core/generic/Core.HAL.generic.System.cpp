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
            if (const char *user = std::getenv("USER")) {
                if (*user != '\0')
                    return std::string(user);
            }

            if (const char *user = std::getenv("USERNAME")) {
                if (*user != '\0')
                    return std::string(user);
            }

            return "unknown_user";
        }();

        return g_username;
    }
}
