module;

#include "pP/Macros.h"

module engine.core;

import :assert;
import :hal;
import :memory;

import std;

namespace pP::hal {
    [[nodiscard]] std::size_t transcode(const std::string_view ansi, char8_t *p_dst, const std::size_t capacity) noexcept {
        const std::size_t n = std::min(ansi.size(), capacity);
        std::memcpy(p_dst, ansi.data(), n * sizeof(char8_t));
        return n;
    }

    [[nodiscard]] std::size_t transcode(const std::string_view ansi, wchar_t *p_dst, const std::size_t capacity) noexcept {
        const std::size_t n = std::min(ansi.size(), capacity);
        for (std::size_t i = 0; i < n; ++i) {
            p_dst[i] = static_cast<wchar_t>(static_cast<unsigned char>(ansi[i]));
        }
        return n;
    }

    [[nodiscard]] std::size_t transcode(const std::u8string_view utf8, wchar_t *p_dst, const std::size_t capacity) noexcept {
        const std::size_t n = std::min(utf8.size(), capacity);
        for (std::size_t i = 0; i < n; ++i) {
            p_dst[i] = static_cast<wchar_t>(utf8[i]);
        }
        return n;
    }

    [[nodiscard]] std::size_t transcode(const std::wstring_view wide, char8_t *p_dst, const std::size_t capacity) noexcept {
        const std::size_t n = std::min(wide.size(), capacity);
        for (std::size_t i = 0; i < n; ++i) {
            p_dst[i] = static_cast<char8_t>(wide[i] & 0xFF);
        }
        return n;
    }

    [[nodiscard]] std::size_t transcode(const std::wstring_view wide, char *const p_dst, const std::size_t capacity) noexcept {
        const std::size_t n = std::min(wide.size(), capacity);
        for (std::size_t i = 0; i < n; ++i) {
            p_dst[i] = static_cast<char>(wide[i] & 0xFF);
        }
        return n;
    }

    [[nodiscard]] std::size_t transcode(const std::u8string_view utf8, char *const p_dst, const std::size_t capacity) noexcept {
        const std::size_t n = std::min(utf8.size(), capacity);
        std::memcpy(p_dst, utf8.data(), n * sizeof(char));
        return n;
    }
}
