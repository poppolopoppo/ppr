module;
#include "pP/Macros.h"

module engine.core;

import :utility;

import std;

namespace pP {
    std::optional<float> percentile_nearest_rank(
        std::span<const float> samples, std::span<float> scratch, float q) noexcept {
        if (samples.empty()) {
            return std::nullopt;
        }
        PPR_ASSERT(scratch.size() >= samples.size());
        const std::size_t count = samples.size();
        std::copy(samples.begin(), samples.end(), scratch.begin());
        std::sort(scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t>(count));
        // Percent recovery keeps the historical integer rank expression
        // below byte-identical: q = 0.5 recovers 50, q = 0.95 recovers 95.
        const std::size_t percent = static_cast<std::size_t>(
            std::llround(static_cast<double>(clamp(q, 0.0f, 1.0f)) * 100.0));
        std::size_t index = (percent * count + 99u) / 100u;
        index = index > 0u ? index - 1u : 0u;
        if (index >= count) {
            index = count - 1u;
        }
        return scratch[index];
    }
} // namespace pP
