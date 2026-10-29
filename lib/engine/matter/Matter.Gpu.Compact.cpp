module;
#include "pP/Macros.h"

module engine.matter;

import :gpu;
import :gpu.types;
import std;
import engine.core;
import engine.math;

namespace pP::matter {
    // ------------------------------------------------------------------
    // canonical contour table encoding
    // ------------------------------------------------------------------

    Expected<PackedContourTables> packContourTables(
        const std::span<const math::ContourPoint, 8u> points, const std::span<const math::ContourCase, 16u> cases) {
        PackedContourTables result{};
        for (u32 index = 0u; index < points.size(); ++index) {
            const auto point = points[index];
            const bool valid = std::isfinite(point.m_x) and
                               std::isfinite(point.m_y) and
                               point.m_x >= 0.0f and
                               point.m_x <= 1.0f and
                               not std::signbit(point.m_x) and
                               point.m_y >= 0.0f and
                               point.m_y <= 1.0f and
                               not std::signbit(point.m_y) and
                               std::floor(point.m_x * 2.0f) == point.m_x * 2.0f and
                               std::floor(point.m_y * 2.0f) == point.m_y * 2.0f;
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                MatterGpu, valid ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));

            const u32 code = static_cast<u32>(point.m_x * 2.0f) | (static_cast<u32>(point.m_y * 2.0f) << 2u);
            result.m_points |= code << (4u * index);
        }

        for (u32 index = 0u; index < cases.size(); ++index) {
            const auto &shape = cases[index];
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                MatterGpu, shape.m_segment_count <= 2u and shape.m_triangle_count <= 3u
                               ? std::error_code{}
                               : std::make_error_code(std::errc::invalid_argument));

            u64 bits = shape.m_segment_count | (static_cast<u64>(shape.m_triangle_count) << 2u);
            for (u32 segment_index = 0u; segment_index < 2u; ++segment_index) {
                const auto segment = shape.m_segments[segment_index];
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    MatterGpu, segment.m_begin <= 7u and segment.m_end <= 7u
                                   ? std::error_code{}
                                   : std::make_error_code(std::errc::invalid_argument));

                const u32 code = segment.m_begin | (segment.m_end << 3u);
                bits |= static_cast<u64>(code) << (4u + 6u * segment_index);
            }

            for (u32 triangle_index = 0u; triangle_index < 3u; ++triangle_index) {
                const auto triangle = shape.m_triangles[triangle_index];
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    MatterGpu,
                    triangle.m_a <= 7u and triangle.m_b <= 7u and triangle.m_c <= 7u and triangle.m_solid_corners <= 15u
                        ? std::error_code{}
                        : std::make_error_code(std::errc::invalid_argument));

                const u32 code = triangle.m_a |
                                 (triangle.m_b << 3u) |
                                 (triangle.m_c << 6u) |
                                 (triangle.m_solid_corners << 9u);
                bits |= static_cast<u64>(code) << (16u + 13u * triangle_index);
            }

            result.m_cases[index] = {static_cast<u32>(bits), static_cast<u32>(bits >> 32u)};
        }

        return result;
    }

    math::ContourCase unpackContourCase(const PackedContourCase packed) noexcept {
        const u64 bits = packed.m_low | (static_cast<u64>(packed.m_high) << 32u);
        math::ContourCase result{};
        result.m_segment_count = static_cast<u32>(bits & 3u);
        result.m_triangle_count = static_cast<u32>((bits >> 2u) & 3u);
        for (u32 index = 0u; index < 2u; ++index) {
            const u32 code = static_cast<u32>(bits >> (4u + 6u * index));
            result.m_segments[index] = {code & 7u, (code >> 3u) & 7u};
        }

        for (u32 index = 0u; index < 3u; ++index) {
            const u32 code = static_cast<u32>(bits >> (16u + 13u * index));
            result.m_triangles[index] = {code & 7u, (code >> 3u) & 7u, (code >> 6u) & 7u, (code >> 9u) & 15u};
        }

        return result;
    }

    math::ContourPoint unpackContourPoint(const u32 packed, const u32 index) noexcept {
        PPR_ASSERT(index < 8u);
        const u32 code = packed >> (4u * index);
        return {static_cast<float>(code & 3u) * 0.5f, static_cast<float>((code >> 2u) & 3u) * 0.5f};
    }

    // ------------------------------------------------------------------
    // immutable terrain upload words
    // ------------------------------------------------------------------

    Expected<PackedTerrain> packTerrain(const TerrainView terrain) {
        const u64 cells = static_cast<u64>(terrain.m_width) * terrain.m_height;
        const bool present = not terrain.m_masks.empty();
        const bool valid = present ? terrain.m_width != 0u and
                                         terrain.m_height != 0u and
                                         cells == terrain.m_masks.size() and
                                         terrain.m_width <= (1u << 22u) and
                                         terrain.m_height <= (1u << 22u) and
                                         cells <= std::numeric_limits<u32>::max()
                                   : terrain.m_width == 0u and terrain.m_height == 0u;
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            MatterGpu, valid ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));

        const u32 columns = (terrain.m_width + 7u) / 8u;
        const u64 tiles = static_cast<u64>(columns) * ((terrain.m_height + 7u) / 8u);
        PackedTerrain result;
        result.m_masks.resize(static_cast<std::size_t>((cells + 7u) / 8u), 0u);
        result.m_boundary_tiles.resize(static_cast<std::size_t>((tiles + 31u) / 32u), 0u);
        const auto cases = math::contourCases();

        for (std::size_t index = 0u; index < terrain.m_masks.size(); ++index) {
            const u32 mask = terrain.m_masks[index];
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                MatterGpu, mask <= 15u ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));

            result.m_masks[index / 8u] |= mask << (4u * static_cast<u32>(index % 8u));
            if (cases[mask].m_segment_count != 0u) {
                const auto x = static_cast<u32>(index % terrain.m_width);
                const auto y = static_cast<u32>(index / terrain.m_width);
                const u64 tile = static_cast<u64>(y / 8u) * columns + x / 8u;
                result.m_boundary_tiles[static_cast<std::size_t>(tile / 32u)] |= 1u << (tile % 32u);
            }
        }

        return result;
    }
}
