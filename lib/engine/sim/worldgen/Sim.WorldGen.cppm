module;

export module engine.sim:worldgen;

import :chunk_grid;

import engine.core;

import std;

export namespace pP::sim {
    enum class EGenerationStage : u8 {
        layout,
        terrain,
        caves,
        initial_matter,
        validation,
        complete
    };
    /// Borrowed execution dependencies, valid until generation has drained.
    /// A null pool selects serial execution on the caller (the game uses its
    /// coordinator, never the UI). Progress counts completed whole chunks.
    struct GenerationExecution {
        ParallelScheduler *m_pool{};
        const IContext *m_context{};
        std::atomic<u32> *m_completed_chunks{};
        std::atomic<EGenerationStage> *m_stage{};
    };

    [[nodiscard]] std::error_code generate(u64 seed, ChunkGrid &grid, const GenerationExecution &execution);

    // ------------------------------------------------------------------
    // deterministic offline world generation
    // ------------------------------------------------------------------

    /// Generates the full fixed cell world from `seed` into the caller-owned
    /// `grid`. Pure in `(seed, grid)`: a seeded integer hash plus fixed grid
    /// dimensions are the only inputs — no `random_device`, no wall-clock, no
    /// thread timing — so the same seed always yields a bit-identical grid.
    /// Generation is offline (not per-tick): every written chunk ends up dirty
    /// through the existing `setCell` path, so the result is quiescent-friendly
    /// and directly consumable by `capture`.
    ///
    /// Per-cell payload is only `(element id, temperature)`; the `Cell` shape
    /// is untouched. Element ids are opaque `u16` here — the sim never names
    /// biomes or materials. The canonical id registry (including the vacuum
    /// id 0 convention) lives colony-side in `game/colony/Colony.Elements.h`;
    /// the numeric ids assigned below must match that registry exactly.
    void generate(u64 seed, ChunkGrid &grid);
}
