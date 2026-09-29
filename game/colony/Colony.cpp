module game.colony;

import engine.core;
import engine.sim;
import game.colony.worldgen;

import std;

namespace pP::colony {
    const sim::ChunkGrid &Colony::grid() const noexcept {
        return m_grid;
    }

    sim::ChunkGrid &Colony::grid() noexcept {
        return m_grid;
    }

    bool Colony::isInitialized() const noexcept {
        return m_initialized;
    }

    std::error_code Colony::init(const ColonyDesc &desc) {
        const std::error_code err = generateWorld(desc.m_seed, m_grid);
        if (err) [[unlikely]] {
            return err;
        }

        m_initialized = true;
        return {};
    }

    std::error_code Colony::shutdown() {
        m_grid = sim::ChunkGrid{};
        m_initialized = false;
        return {};
    }
}
