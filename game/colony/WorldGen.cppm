export module game.colony.worldgen;

import engine.core;
import engine.sim;

import std;

export namespace pP::colony {
    /// Thin wrapper over the sim-side deterministic generator
    /// (`sim::generate`): game code submits work through this boundary and
    /// never touches the generator internals. Infallible in practice — the
    /// generator writes in-bounds cells with finite temperatures only — but
    /// reports `error_code` to keep the operational boundary explicit.
    [[nodiscard]] std::error_code generateWorld(u64 seed, sim::ChunkGrid &grid);
}
