module;

export module engine.sim:worldgen;

export import :worldgen.definition;

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
        ParallelScheduler *m_scheduler{&parallel::backgroundPool()};
        SharedContext m_context{};
        std::atomic<u32> *m_completed_chunks{};
        std::atomic<EGenerationStage> *m_stage{};
    };

}
