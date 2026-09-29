module;
#include "pP/Macros.h"

module engine.sim;

import :worldgen;

import :chunk_grid;

import engine.core;

import std;

namespace pP::sim {
    namespace WorldGen {
        // ------------------------------------------------------------------
        // opaque numeric element ids (canonical registry: Colony.Elements.h)
        // ------------------------------------------------------------------

        inline constexpr u16 kVacuum = 0u;
        inline constexpr u16 kBase = 1u;
        inline constexpr u16 kFrost = 2u;
        inline constexpr u16 kPocket = 3u;
        inline constexpr u16 kLens = 4u;
        inline constexpr u16 kDistrict = 5u;
        inline constexpr u16 kBorder = 6u;
        inline constexpr u16 kVent = 7u;

        // ------------------------------------------------------------------
        // fixed layout geometry (seed-independent, so archetype presence is
        // geometric and holds for every seed; the seed only moves the pocket,
        // lens, district, vents, and tunnels and feathers region rims)
        // ------------------------------------------------------------------

        inline constexpr u32 kBorderWidth = 48u;
        inline constexpr u32 kFrostCore = 256u;
        inline constexpr u32 kFrostFeather = 704u;
        inline constexpr u32 kVentCount = 6u;
        inline constexpr u32 kWormCount = 16u;

        // ------------------------------------------------------------------
        // self-contained seeded hashing (splitmix64; the core
        // `randomNumberGenerator()` is hardware-seeded and `RngStream` is
        // snapshot-local, so neither is usable here)
        // ------------------------------------------------------------------

        [[nodiscard]] constexpr u64 mixOnce(u64 value) noexcept {
            value ^= value >> 30;
            value *= 0xBF58476D1CE4E5B9ULL;
            value ^= value >> 27;
            value *= 0x94D049BB133111EBULL;
            return value ^ (value >> 31);
        }

        [[nodiscard]] constexpr u64 nextStream(u64 &state) noexcept {
            state += 0x9E3779B97F4A7C15ULL;
            return mixOnce(state);
        }

        [[nodiscard]] constexpr u64 hashCell(const u32 x, const u32 y, const u64 seed) noexcept {
            const u64 packed = (static_cast<u64>(x) << 32) | y;
            return mixOnce(seed + 0x9E3779B97F4A7C15ULL + packed * 0xBF58476D1CE4E5B9ULL);
        }

        /// White noise in [0, 1): top 53 bits of the cell hash over 2^53,
        /// resolved in `double` so the full 53 bits survive (a `float`
        /// mantissa would quantize the output).
        [[nodiscard]] constexpr float hash01(const u32 x, const u32 y, const u64 seed) noexcept {
            constexpr double kTwoToNeg53 = 1.0 / 9007199254740992.0;
            const double unit = static_cast<double>(hashCell(x, y, seed) >> 11) * kTwoToNeg53;
            return static_cast<float>(unit);
        }

        /// Coherent region-mask noise in [0, 1]: lattice hashes at `scale`
        /// with smoothstep bilinear interpolation.
        [[nodiscard]] float smooth(const u32 x, const u32 y, const u64 seed, const u32 scale) noexcept {
            const u32 lattice_x = x / scale;
            const u32 lattice_y = y / scale;
            const float frac_x = static_cast<float>(x % scale) / static_cast<float>(scale);
            const float frac_y = static_cast<float>(y % scale) / static_cast<float>(scale);
            const float sx = frac_x * frac_x * (3.0f - 2.0f * frac_x);
            const float sy = frac_y * frac_y * (3.0f - 2.0f * frac_y);

            const float v00 = hash01(lattice_x, lattice_y, seed);
            const float v10 = hash01(lattice_x + 1u, lattice_y, seed);
            const float v01 = hash01(lattice_x, lattice_y + 1u, seed);
            const float v11 = hash01(lattice_x + 1u, lattice_y + 1u, seed);

            const float low = v00 + (v10 - v00) * sx;
            const float high = v01 + (v11 - v01) * sx;
            return low + (high - low) * sy;
        }

        struct Layout final {
            u32 m_pocket_x{};
            u32 m_pocket_y{};
            u32 m_pocket_radius{};
            u32 m_lens_x{};
            u32 m_lens_y{};
            u32 m_lens_rx{};
            u32 m_lens_ry{};
            u32 m_district_x{};
            u32 m_district_y{};
            u32 m_vent_x[kVentCount]{};
            u32 m_vent_y[kVentCount]{};
            u32 m_vent_radius[kVentCount]{};
            u32 m_worm_x[kWormCount]{};
            u32 m_worm_y[kWormCount]{};
            u32 m_worm_steps[kWormCount]{};
            u32 m_worm_angle[kWormCount]{};
        };

        [[nodiscard]] Layout deriveLayout(const u64 seed) noexcept {
            Layout layout{};
            u64 state = seed + 0x9E3779B97F4A7C15ULL;
            layout.m_pocket_x = 1024u + static_cast<u32>(nextStream(state) % 2048u);
            layout.m_pocket_y = 1024u + static_cast<u32>(nextStream(state) % 2048u);
            layout.m_pocket_radius = 240u + static_cast<u32>(nextStream(state) % 145u);

            layout.m_lens_x = 768u + static_cast<u32>(nextStream(state) % 2560u);
            layout.m_lens_y = 2304u + static_cast<u32>(nextStream(state) % 1024u);
            layout.m_lens_rx = 260u + static_cast<u32>(nextStream(state) % 161u);
            layout.m_lens_ry = 120u + static_cast<u32>(nextStream(state) % 81u);

            layout.m_district_x = 640u + static_cast<u32>(nextStream(state) % 1408u);
            layout.m_district_y = 640u + static_cast<u32>(nextStream(state) % 1408u);

            for (u32 index = 0u; index < kVentCount; ++index) {
                layout.m_vent_x[index] = 512u + static_cast<u32>(nextStream(state) % 3072u);
                layout.m_vent_y[index] = 512u + static_cast<u32>(nextStream(state) % 3072u);
                layout.m_vent_radius[index] = 18u + static_cast<u32>(nextStream(state) % 13u);
            }

            for (u32 index = 0u; index < kWormCount; ++index) {
                layout.m_worm_x[index] = 256u + static_cast<u32>(nextStream(state) % 3584u);
                layout.m_worm_y[index] = 256u + static_cast<u32>(nextStream(state) % 3584u);
                layout.m_worm_steps[index] = 500u + static_cast<u32>(nextStream(state) % 601u);
                layout.m_worm_angle[index] = static_cast<u32>(nextStream(state) % 6283u);
            }

            return layout;
        }

        /// Per-generate float cache of `Layout`: the same products and casts
        /// `baseCell` needs, hoisted out of the 16.7M-cell fill loop. Each
        /// field evaluates the identical expression (same operand order) it
        /// replaces, so the generated grid is unchanged.
        struct Tuned final {
            float m_pocket_core2{};
            float m_pocket_rim2{};
            float m_lens_x{};
            float m_lens_y{};
            float m_lens_rx{};
            float m_lens_ry{};
            float m_vent_r2[kVentCount]{};
        };

        [[nodiscard]] Tuned tuneLayout(const Layout &layout) noexcept {
            Tuned tuned{};
            const float pocket_radius = static_cast<float>(layout.m_pocket_radius);
            tuned.m_pocket_core2 = pocket_radius * pocket_radius * 0.3025f;
            tuned.m_pocket_rim2 = pocket_radius * pocket_radius;
            tuned.m_lens_x = static_cast<float>(layout.m_lens_x);
            tuned.m_lens_y = static_cast<float>(layout.m_lens_y);
            tuned.m_lens_rx = static_cast<float>(layout.m_lens_rx);
            tuned.m_lens_ry = static_cast<float>(layout.m_lens_ry);
            for (u32 index = 0u; index < kVentCount; ++index) {
                const float radius = static_cast<float>(layout.m_vent_radius[index]);
                tuned.m_vent_r2[index] = radius * radius;
            }
            return tuned;
        }

        [[nodiscard]] constexpr u32 edgeDistance(const u32 x, const u32 y) noexcept {
            u32 distance = x;
            if (y < distance) {
                distance = y;
            }
            const u32 far_x = kWorldEdge - 1u - x;
            if (far_x < distance) {
                distance = far_x;
            }
            const u32 far_y = kWorldEdge - 1u - y;
            if (far_y < distance) {
                distance = far_y;
            }
            return distance;
        }

        [[nodiscard]] constexpr float squaredDistance(const u32 ax, const u32 ay, const u32 bx,
                                                      const u32 by) noexcept {
            const float dx = static_cast<float>(ax) - static_cast<float>(bx);
            const float dy = static_cast<float>(ay) - static_cast<float>(by);
            return dx * dx + dy * dy;
        }

        [[nodiscard]] Cell baseCell(const u32 x, const u32 y, const u64 seed, const Layout &layout,
                                    const Tuned &tuned) noexcept {
            const u32 edge = edgeDistance(x, y);
            const float white = hash01(x, y, seed);

            // Abyssal border: fixed outer ring, always solid.
            if (edge < kBorderWidth) {
                return {kBorder, 208.0f + (white - 0.5f) * 10.0f};
            }

            // Geyser vents: unconditional hot discs at seeded positions.
            for (u32 index = 0u; index < kVentCount; ++index) {
                const bool inside = squaredDistance(x, y, layout.m_vent_x[index], layout.m_vent_y[index]) <
                                    tuned.m_vent_r2[index];
                if (inside) {
                    return {kVent, 402.0f + (white - 0.5f) * 24.0f};
                }
            }

            // Caustic pocket: unconditional core, noise-feathered rim. The rim
            // `smooth` lookup runs only when the rim test it feeds can observe
            // it: outside the rim (or inside the core) its result is discarded
            // by the short-circuit, so guarding it leaves the grid identical
            // while skipping ~16M lattice lookups per generate.
            {
                const float dist2 = squaredDistance(x, y, layout.m_pocket_x, layout.m_pocket_y);
                if (dist2 < tuned.m_pocket_core2) {
                    return {kPocket, 332.0f + (white - 0.5f) * 16.0f};
                }
                if (dist2 < tuned.m_pocket_rim2 and smooth(x, y, seed ^ 0xC0A57CuLL, 128u) > 0.35f) {
                    return {kPocket, 332.0f + (white - 0.5f) * 16.0f};
                }
            }

            // Oil reservoir: unconditional lens core, noise-feathered rim
            // (same short-circuit guard as the pocket).
            {
                const float nx = (static_cast<float>(x) - tuned.m_lens_x) / tuned.m_lens_rx;
                const float ny = (static_cast<float>(y) - tuned.m_lens_y) / tuned.m_lens_ry;
                const float ellipse = nx * nx + ny * ny;
                if (ellipse < 0.36f) {
                    return {kLens, 284.0f + (white - 0.5f) * 6.0f};
                }
                if (ellipse < 1.0f and smooth(x, y, seed ^ 0x0115EEDuLL, 128u) > 0.30f) {
                    return {kLens, 284.0f + (white - 0.5f) * 6.0f};
                }
            }

            // Frozen edge: unconditional inner band, noise-feathered outer
            // band. Cells already inside the core skip the feather lookup
            // their branch cannot observe.
            if (edge < kFrostCore) {
                return {kFrost, 228.0f + (white - 0.5f) * 12.0f};
            }
            if (edge < kFrostFeather and smooth(x, y, seed ^ 0xF8057uLL, 256u) > 0.42f) {
                return {kFrost, 228.0f + (white - 0.5f) * 12.0f};
            }

            // Ruins: fixed-size seeded district with street grid; blocks are
            // ruins, streets keep the temperate base. The `% 44` street tests
            // run only inside the district bounding box that gates them.
            if (x >= layout.m_district_x and
                x < layout.m_district_x + 768u and
                y >= layout.m_district_y and
                y < layout.m_district_y + 768u and
                (x % 44u) < 30u and
                (y % 44u) < 30u) {
                return {kDistrict, 296.0f + (white - 0.5f) * 4.0f};
            }

            // Temperate core: the default fill.
            return {kBase, 293.0f + (white - 0.5f) * 6.0f};
        }

        void carveDisc(ChunkGrid &grid, const u32 center_x, const u32 center_y, const u32 radius) {
            const float disc_r2 = static_cast<float>(radius * radius);
            for (u32 dy = 0u; dy <= 2u * radius; ++dy) {
                for (u32 dx = 0u; dx <= 2u * radius; ++dx) {
                    const i32 x = static_cast<i32>(center_x) - static_cast<i32>(radius) + static_cast<i32>(dx);
                    const i32 y = static_cast<i32>(center_y) - static_cast<i32>(radius) + static_cast<i32>(dy);
                    const bool negative = x < 0 or y < 0;
                    const bool past_edge = x >= static_cast<i32>(kWorldEdge) or y >= static_cast<i32>(kWorldEdge);
                    if (negative or past_edge) {
                        continue;
                    }

                    const auto ux = static_cast<u32>(x);
                    const auto uy = static_cast<u32>(y);
                    const bool in_disc = squaredDistance(ux, uy, center_x, center_y) <= disc_r2;
                    const bool in_border = edgeDistance(ux, uy) < kBorderWidth;
                    if (not in_disc or in_border) {
                        continue;
                    }

                    const GlobalCellPos pos{ux, uy};
                    const Expected<Cell> prior = grid.getCell(pos);
                    if (not prior.has_value()) [[unlikely]] {
                        continue;
                    }
                    Cell carved = *prior;
                    carved.m_element = kVacuum;
                    PPR_VERIFY(not grid.setCell(pos, carved));
                }
            }
        }

        // Cavern-worm compass: 16 headings of stride 3.0, replacing the old
        // `3.0 * (cos, sin)` step. Transcendental libm calls are not
        // bit-identical across platforms/toolchains; this table plus integer
        // heading updates keep `generate` a pure function of (seed, grid).
        inline constexpr float kWormStep[16][2] = {
            {3.0f, 0.0f},
            {2.7716386f, 1.1480503f},
            {2.1213203f, 2.1213203f},
            {1.1480503f, 2.7716386f},
            {0.0f, 3.0f},
            {-1.1480503f, 2.7716386f},
            {-2.1213203f, 2.1213203f},
            {-2.7716386f, 1.1480503f},
            {-3.0f, 0.0f},
            {-2.7716386f, -1.1480503f},
            {-2.1213203f, -2.1213203f},
            {-1.1480503f, -2.7716386f},
            {0.0f, -3.0f},
            {1.1480503f, -2.7716386f},
            {2.1213203f, -2.1213203f},
            {2.7716386f, -1.1480503f},
        };

        void carveCaverns(ChunkGrid &grid, const u64 seed, const Layout &layout) {
            const u64 turn_seed = seed ^ 0xB09A11uLL;
            const u64 radius_seed = seed ^ 0xCA9E0uLL;
            for (u32 worm = 0u; worm < kWormCount; ++worm) {
                float px = static_cast<float>(layout.m_worm_x[worm]);
                float py = static_cast<float>(layout.m_worm_y[worm]);
                i32 heading = static_cast<i32>((layout.m_worm_angle[worm] * 16u) / 6283u % 16u);

                for (u32 step = 0u; step < layout.m_worm_steps[worm]; ++step) {
                    // Same hashed turn stream as before (`hash01` in
                    // [-0.5, 0.5)); a turn beyond half a compass step
                    // (0.5 * (pi/8) / 0.9 ~= 0.21816616) nudges the heading
                    // by one, mirroring the old +-0.45 rad/step curvature.
                    const float turn = hash01(step, worm, turn_seed) - 0.5f;
                    i32 nudge = 0;
                    if (turn > 0.21816616f) {
                        nudge = 1;
                    } else if (turn < -0.21816616f) {
                        nudge = -1;
                    }
                    heading = (heading + nudge + 16) & 15;
                    px += kWormStep[heading][0];
                    py += kWormStep[heading][1];

                    const bool non_negative = px >= 0.0f and py >= 0.0f;
                    const bool within_edge = px < static_cast<float>(kWorldEdge) and
                    py < static_cast<float>(kWorldEdge);
                    if (not non_negative or not within_edge) {
                        break;
                    }

                    const u32 radius = 2u + static_cast<u32>(hash01(step, worm, radius_seed) * 3.0f);
                    carveDisc(grid, static_cast<u32>(px), static_cast<u32>(py), radius);
                }
            }
        }

        void carveVentShafts(ChunkGrid &grid, const Layout &layout) {
            for (u32 index = 0u; index < kVentCount; ++index) {
                const u32 vent_x = layout.m_vent_x[index];
                const u32 vent_y = layout.m_vent_y[index];
                const u32 top = vent_y > 260u ? vent_y - 260u : 0u;
                for (u32 y = top; y < vent_y; ++y) {
                    for (u32 dx = 0u; dx <= 6u; ++dx) {
                        const i32 x = static_cast<i32>(vent_x) - 3 + static_cast<i32>(dx);
                        if (x < 0 or x >= static_cast<i32>(kWorldEdge)) {
                            continue;
                        }
                        const auto ux = static_cast<u32>(x);
                        if (edgeDistance(ux, y) < kBorderWidth) {
                            continue;
                        }
                        const GlobalCellPos pos{ux, y};
                        const Expected<Cell> prior = grid.getCell(pos);
                        if (not prior.has_value()) [[unlikely]] {
                            continue;
                        }
                        Cell carved = *prior;
                        carved.m_element = kVacuum;
                        PPR_VERIFY(not grid.setCell(pos, carved));
                    }
                }
            }
        }
    } // namespace WorldGen

    void generate(const u64 seed, ChunkGrid &grid) {
        const WorldGen::Layout layout = WorldGen::deriveLayout(seed);
        const WorldGen::Tuned tuned = WorldGen::tuneLayout(layout);

        for (u32 y = 0u; y < kWorldEdge; ++y) {
            for (u32 x = 0u; x < kWorldEdge; ++x) {
                const Cell cell = WorldGen::baseCell(x, y, seed, layout, tuned);
                PPR_VERIFY(not grid.setCell(GlobalCellPos{x, y}, cell));
            }
        }

        WorldGen::carveCaverns(grid, seed, layout);
        WorldGen::carveVentShafts(grid, layout);
    }
}
