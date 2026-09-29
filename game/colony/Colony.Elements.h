#pragma once

// Canonical element-id registry for the colony world generator.
//
// The sim never names biomes: `engine.sim:worldgen` assigns only opaque `u16`
// ids, and the numeric values below must match those assignments exactly.
// Vacuum is id 0 by convention — confirmed on disk by the physics collider
// solid test (`chunk->m_elements[...] != 0u` in
// `lib/engine/physics/Physics.ChunkColliders.cpp`) and by the `Cell{}`
// default (a non-resident chunk reads back as all-vacuum).
//
// Plain header (not a module): colony fixture code and tests include it
// without taking a module dependency. Uses fixed-width integers only.

#include <cstdint>

namespace pP::colony {
    inline constexpr std::uint16_t kElementVacuum = 0u;
    inline constexpr std::uint16_t kElementRock = 1u;
    inline constexpr std::uint16_t kElementIce = 2u;
    inline constexpr std::uint16_t kElementCaustic = 3u;
    inline constexpr std::uint16_t kElementOil = 4u;
    inline constexpr std::uint16_t kElementRuins = 5u;
    inline constexpr std::uint16_t kElementAbyssal = 6u;
    inline constexpr std::uint16_t kElementVent = 7u;

    // Nominal (noise-free) temperatures in Kelvin assigned per archetype.
    inline constexpr float kTempRock = 293.0f;
    inline constexpr float kTempIce = 228.0f;
    inline constexpr float kTempCaustic = 332.0f;
    inline constexpr float kTempOil = 284.0f;
    inline constexpr float kTempRuins = 296.0f;
    inline constexpr float kTempAbyssal = 208.0f;
    inline constexpr float kTempVent = 402.0f;
} // namespace pP::colony
