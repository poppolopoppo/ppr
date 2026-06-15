module;

#include "pP/Macros.h"

module engine.core;

import :assert;
import :hal;
import :memory;

import std;

namespace pP::hal::io {
    struct MapHandleData { void *m_data{}; std::size_t m_size{}; };

    MapHandle mapFile(const std::filesystem::path &, const OpenFlags) noexcept(false) {
        throw std::system_error(
            std::make_error_code(std::errc::operation_not_supported),
            "pP::io: memory-mapped files not supported on this platform");
    }

    void unmapFile(const MapHandle) noexcept {}

    void *mapData(const MapHandle) noexcept { return nullptr; }
    std::size_t mapSize(const MapHandle) noexcept { return 0u; }
}
