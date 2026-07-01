module;

#include "pP/Macros.h"

module engine.core;

import :hal;

import std;

namespace pP {
    std::mt19937_64 randomNumberGenerator() noexcept {
        std::array<std::uint32_t, 8> seed_data{};
        std::random_device rd;

        for (auto &x: seed_data) {
            x = rd();
        }

        std::seed_seq seq(seed_data.begin(), seed_data.end());
        return std::mt19937_64(seq);
    }
}
