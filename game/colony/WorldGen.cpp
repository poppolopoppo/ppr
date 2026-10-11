module game.colony.worldgen;

import engine.core;
import engine.sim;

import std;

namespace pP::colony {
    std::error_code generateWorld(
        const u64 seed, sim::ChunkGrid &grid, const sim::GenerationExecution *const execution) {
        if (execution != nullptr) {
            return sim::generate(seed, grid, *execution);
        }
        sim::generate(seed, grid);
        return {};
    }
}
