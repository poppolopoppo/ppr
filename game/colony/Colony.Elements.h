#pragma once

// Fixed numeric material IDs for explicit colony fixtures and smoke scenarios.
// Authored worlds resolve material IDs through ContentCatalog; these constants
// describe only the fixture contract. Vacuum remains zero, matching Cell{}.
// A plain header lets fixture code and tests share IDs without a module edge.

#include <cstdint>

namespace pP::colony {
    // ------------------------------------------------------------------
    // colony elements
    // ------------------------------------------------------------------

    inline constexpr std::uint16_t kElementVacuum = 0u;
    inline constexpr std::uint16_t kElementRock = 1u;
    inline constexpr std::uint16_t kElementIce = 2u;
    inline constexpr std::uint16_t kElementCaustic = 3u;
    inline constexpr std::uint16_t kElementOil = 4u;
    inline constexpr std::uint16_t kElementRuins = 5u;
    inline constexpr std::uint16_t kElementAbyssal = 6u;
    inline constexpr std::uint16_t kElementVent = 7u;

} // namespace pP::colony
