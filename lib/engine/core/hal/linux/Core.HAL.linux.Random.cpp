module;

#include "pP/Macros.h"

module engine.core;

import std;

namespace pP {
    [[nodiscard]] std::mt19937_64 randomNumberGenerator() noexcept {
        std::array<std::uint32_t, 8u> seed_data{};
        std::random_device seed_source{};
        for (auto &word : seed_data) {
            word = seed_source();
        }
        std::seed_seq seed(seed_data.begin(), seed_data.end());
        return std::mt19937_64(seed);
    }
}
