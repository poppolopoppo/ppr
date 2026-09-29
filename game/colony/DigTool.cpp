module;

#include "pP/Macros.h"
#include "Colony.Elements.h"

module game.colony.digtool;

import engine.core;
import engine.sim;

import std;

namespace pP::colony {
    namespace {
        [[nodiscard]] bool chunkListed(const Array<sim::ChunkPos> &touched, const sim::ChunkPos pos) noexcept {
            for (const sim::ChunkPos listed: touched) {
                if (listed == pos) {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] std::error_code validateDigRect(const DigRect &rect) noexcept {
            const bool bad_edge = rect.m_width == 0u or
            rect.m_height == 0u
            or
            rect.m_width > kMaxDigEdge
            or
            rect.m_height > kMaxDigEdge;
            if (bad_edge) {
                return std::make_error_code(std::errc::invalid_argument);
            }
            const u64 area = static_cast<u64>(rect.m_width) * rect.m_height;
            if (area > kMaxDigCells) {
                return std::make_error_code(std::errc::invalid_argument);
            }
            const u64 far_x = static_cast<u64>(rect.m_min.m_x) + rect.m_width;
            const u64 far_y = static_cast<u64>(rect.m_min.m_y) + rect.m_height;
            if (far_x > sim::kWorldEdge
                or
                        far_y > sim::kWorldEdge)
            {
                return std::make_error_code(std::errc::invalid_argument);
            }
            return {};
        }

        struct DigTarget {
            u32 m_x{};
            u32 m_y{};
            float m_temperature{};
        };
    }

    Expected<DigResult> digCells(sim::ChunkGrid &grid, const DigRect &rect) {
        if (const std::error_code error = validateDigRect(rect)) {
            return std::unexpected{error};
        }

        Array<DigTarget> targets{};
        for (u32 row = 0u; row < rect.m_height; ++row) {
            for (u32 col = 0u; col < rect.m_width; ++col) {
                const u32 x = rect.m_min.m_x + col;
                const u32 y = rect.m_min.m_y + row;
                const auto current = grid.getCell({x, y});
                PPR_ASSERT(current.has_value());
                if (current->m_element == kElementVacuum) {
                    // Already vacuum: no write (so `setCell`'s dirty contract
                    // is never charged), no budget, no touched chunk.
                    continue;
                }
                if (targets.size() >= kMaxDigCells) {
                    return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
                }
                targets.push_back({x, y, current->m_temperature});
            }
        }

        DigResult result{};
        for (const DigTarget target: targets) {
            const std::error_code set_error = grid.setCell({target.m_x, target.m_y},
                sim::Cell{kElementVacuum, target.m_temperature});
            PPR_ASSERT(not set_error);
            const sim::ChunkPos chunk = sim::chunkOf({target.m_x, target.m_y});
            if (not chunkListed(result.m_touched, chunk)) {
                result.m_touched.push_back(chunk);
            }
            ++result.m_dug;
        }
        return result;
    }
}
