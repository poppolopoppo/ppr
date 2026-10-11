module;
#include "pP/Macros.h"

module engine.math;

import engine.core;
import std;

namespace pP::math {
    namespace {
        constexpr std::array<ContourPoint, 8> kPoints{
            {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}, {0.5f, 0.0f}, {1.0f, 0.5f}, {0.5f, 1.0f},
                {0.0f, 0.5f}}};

        constexpr ContourCase polygon(
            const u32 solid_corners, const std::initializer_list<u32> vertices,
            const std::initializer_list<ContourSegment> segments) {
            ContourCase result{};
            result.m_segment_count = static_cast<u32>(segments.size());
            u32 segment_index = 0u;
            for (const auto segment: segments) {
                result.m_segments[segment_index++] = {segment.m_end, segment.m_begin};
            }
            const auto first = vertices.begin();
            for (std::size_t i = 1u; i + 1u < vertices.size(); ++i) {
                result.m_triangles[result.m_triangle_count++] = {first[0], first[i], first[i + 1u], solid_corners};
            }
            return result;
        }

        constexpr ContourCase saddle(const u32 bits) {
            ContourCase result{};
            result.m_segment_count = 2u;
            result.m_triangle_count = 2u;
            if (bits == 5u) {
                result.m_segments = {{{4u, 7u}, {6u, 5u}}};
                result.m_triangles = {{{0u, 4u, 7u, 1u}, {2u, 6u, 5u, 4u}, {}}};
            } else {
                result.m_segments = {{{5u, 4u}, {7u, 6u}}};
                result.m_triangles = {{{1u, 5u, 4u, 2u}, {3u, 7u, 6u, 8u}, {}}};
            }
            return result;
        }

        constexpr std::array<ContourCase, 16> kCases{
            {{}, polygon(1u, {0u, 4u, 7u}, {{7u, 4u}}), polygon(2u, {1u, 5u, 4u}, {{4u, 5u}}),
                polygon(3u, {0u, 1u, 5u, 7u}, {{7u, 5u}}), polygon(4u, {2u, 6u, 5u}, {{5u, 6u}}), saddle(5u),
                polygon(6u, {4u, 1u, 2u, 6u}, {{4u, 6u}}), polygon(7u, {0u, 1u, 2u, 6u, 7u}, {{7u, 6u}}),
                polygon(8u, {3u, 7u, 6u}, {{6u, 7u}}), polygon(9u, {0u, 4u, 6u, 3u}, {{6u, 4u}}), saddle(10u),
                polygon(11u, {0u, 1u, 5u, 6u, 3u}, {{6u, 5u}}), polygon(12u, {7u, 5u, 2u, 3u}, {{5u, 7u}}),
                polygon(13u, {0u, 4u, 5u, 2u, 3u}, {{5u, 4u}}), polygon(14u, {4u, 1u, 2u, 3u, 7u}, {{4u, 7u}}),
                polygon(15u, {0u, 1u, 2u, 3u}, {})}};
    }

    std::span<const ContourPoint, 8> contourPoints() noexcept {
        return kPoints;
    }
    std::span<const ContourCase, 16> contourCases() noexcept {
        return kCases;
    }
}
