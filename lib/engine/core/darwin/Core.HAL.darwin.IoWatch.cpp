module;

#include "pP/Macros.h"

module engine.core;

import :assert;
import :hal;
import :memory;

import std;

namespace pP::hal::io {
    WatchHandle openWatch(const std::filesystem::path &, bool) noexcept(false) {
        throw std::system_error(
            std::make_error_code(std::errc::operation_not_supported),
            "pP::io: directory watching not implemented on darwin");
    }

    void closeWatch(const WatchHandle) noexcept {
    }

    std::size_t pollWatch(const WatchHandle, const std::span<std::byte>, std::error_code &) noexcept {
        return 0u;
    }

    std::size_t waitWatch(const WatchHandle, const std::span<std::byte>, std::error_code &) noexcept {
        return 0u;
    }

    std::size_t parseWatchEvents(const std::span<const std::byte>,
                                  const std::span<WatchEvent>,
                                  const std::span<char>) noexcept {
        return 0u;
    }
}
