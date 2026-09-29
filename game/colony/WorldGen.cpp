module game.colony.worldgen;

import engine.core;
import engine.sim;

import std;

namespace pP::colony {
    std::error_code generateWorld(const u64 seed, sim::ChunkGrid &grid) {
        sim::generate(seed, grid);
        return {};
    }
}
