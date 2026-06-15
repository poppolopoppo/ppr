module;

#include "pP/Macros.h"

module engine.core;

import :hal;

import std;

namespace pP::hal::process {
    [[nodiscard]] std::filesystem::path currentExecutablePath() noexcept(false) {
        throw std::runtime_error("currentExecutablePath not implemented for generic platform");
    }

    [[nodiscard]] int spawnAndWait(const std::filesystem::path &executable, std::span<const std::string> args) noexcept(false) {
        (void)executable;
        (void)args;
        throw std::runtime_error("spawnAndWait not implemented for generic platform");
    }
}
