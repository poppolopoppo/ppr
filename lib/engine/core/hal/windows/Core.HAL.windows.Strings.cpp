module;

#include "Core.HAL.windows.include.hpp"

module engine.core;

import :assert;
import :hal;
import :memory;

import std;

namespace pP::hal {
    [[nodiscard]] std::size_t transcode(const std::string_view ansi, char8_t *const p_dst, const std::size_t capacity) noexcept {
        static_assert(sizeof(char8_t) == sizeof(char));
        const std::size_t n_chars = std::min(ansi.size(), capacity);
        memcpy(p_dst, ansi.data(), n_chars * sizeof(char8_t));
        return n_chars;
    }

    [[nodiscard]] std::size_t transcode(const std::string_view ansi, wchar_t *p_dst, const std::size_t capacity) noexcept {
        const int n_chars = ::MultiByteToWideChar(
            CP_ACP, 0,
            ansi.data(), static_cast<int>(ansi.size()),
            p_dst, static_cast<int>(capacity * sizeof(p_dst[0])));
        if (n_chars == 0) {
            return static_cast<std::size_t>(::MultiByteToWideChar(CP_ACP, 0, ansi.data(), static_cast<int>(ansi.size()), nullptr, 0));
        }
        return static_cast<std::size_t>(n_chars);
    }

    [[nodiscard]] std::size_t transcode(const std::u8string_view utf8, wchar_t *p_dst, const std::size_t capacity) noexcept {
        static_assert(sizeof(*LPCCH{}) == sizeof(char8_t));
        const int n_chars = ::MultiByteToWideChar(
            CP_UTF8, 0,
            reinterpret_cast<LPCCH>(utf8.data()), static_cast<int>(utf8.size()),
            p_dst, static_cast<int>(capacity * sizeof(p_dst[0])));
        if (n_chars == 0) {
            return static_cast<std::size_t>(::MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<LPCCH>(utf8.data()), static_cast<int>(utf8.size()), nullptr, 0));
        }
        return static_cast<std::size_t>(n_chars);
    }

    [[nodiscard]] std::size_t transcode(const std::wstring_view wide, char8_t *p_dst, const std::size_t capacity) noexcept {
        const int n_bytes = ::WideCharToMultiByte(
            CP_UTF8, 0,
            wide.data(), static_cast<int>(wide.size()),
            reinterpret_cast<LPSTR>(p_dst), static_cast<int>(capacity * sizeof(p_dst[0])),
            nullptr, nullptr);
        if (n_bytes == 0) {
            return static_cast<std::size_t>(::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr));
        }
        return static_cast<std::size_t>(n_bytes);
    }

    [[nodiscard]] std::size_t transcode(const std::wstring_view wide, char *const p_dst, const std::size_t capacity) noexcept {
        const int n_bytes = ::WideCharToMultiByte(
            CP_ACP, 0,
            wide.data(), static_cast<int>(wide.size()),
            p_dst, static_cast<int>(capacity * sizeof(p_dst[0])),
            nullptr, nullptr);
        if (n_bytes == 0) {
            return static_cast<std::size_t>(::WideCharToMultiByte(CP_ACP, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr));
        }
        return static_cast<std::size_t>(n_bytes);
    }

    [[nodiscard]] std::size_t transcode(const std::u8string_view utf8, char *const p_dst, const std::size_t capacity) noexcept {
        const std::wstring wide = toString<wchar_t>(utf8);
        return transcode(wide, p_dst, capacity);
    }
}
