module;

// ------------------------------------------------------------------
// config.environment
// ------------------------------------------------------------------

#include "pP/Macros.h"
#include <cstdlib>

module engine.core;

// ------------------------------------------------------------------
// config.environment
// ------------------------------------------------------------------

import :config;
import :config.environment;

import std;

// ------------------------------------------------------------------
// config.environment
// ------------------------------------------------------------------

namespace pP::config {
    [[nodiscard]] std::string environmentName(const std::string_view canonical) {
        std::string result{"PPR_"};
        for (const char ch: canonical) {
            if (ch == '.') {
                result += "__";
            } else if (ch == '-') {
                result += '_';
            } else if (ch >= 'a' and ch <= 'z') {
                result += static_cast<char>(ch - ('a' - 'A'));
            } else {
                result += ch;
            }
        }
        return result;
    }

    [[nodiscard]] Layer environmentLayer(Array<Assignment> assignments, std::string origin) {
        Layer layer{};
        layer.m_source = ESource::environment;
        layer.m_origin = std::move(origin);
        layer.m_assignments = std::move(assignments);
        return layer;
    }

    [[nodiscard]] std::optional<std::string> getenvLookup(const std::string_view name) {
        const std::string key{name};
#if defined(_MSC_VER)
        char *raw = nullptr;
        std::size_t size = 0;
        if (_dupenv_s(std::addressof(raw), std::addressof(size), key.c_str()) != 0) {
            std::free(raw);
            return std::nullopt;
        }

        // RAII owner: a throwing std::string construction still frees.
        const std::unique_ptr<char, decltype(&std::free)> owner{raw, &std::free};
        if (owner == nullptr) {
            return std::nullopt;
        }
        return std::string{owner.get()};
#else
        if (const char *value = std::getenv(key.c_str()); value != nullptr) {
            return std::string{value};
        }
        return std::nullopt;
#endif
    }
} // namespace pP::config
