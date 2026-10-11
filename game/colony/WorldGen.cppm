export module game.colony.worldgen;

import engine.core;
import engine.sim;
import game.colony.content;

import std;

export namespace pP::colony {
    /// Resolves authoring references once into an owned, validated numeric plan.
    [[nodiscard]] Expected<sim::GenerationPlan> compileWorldPlan(
        const ContentCatalog &catalog, std::string_view profile, u64 seed);
    /// Thin wrapper over the sim-side deterministic generator
    /// (`sim::generate`): game code submits work through this boundary and
    /// never touches the generator internals. Infallible in practice — the
    /// generator writes in-bounds cells with finite temperatures only — but
    /// reports `error_code` to keep the operational boundary explicit.
    [[nodiscard]] std::error_code generateWorld(
        u64 seed, sim::ChunkGrid &grid, const sim::GenerationExecution *execution = nullptr);
}
