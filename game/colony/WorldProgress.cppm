module;

export module game.colony.world_progress;

import engine.core;
import std;

export namespace pP::colony {
    // ------------------------------------------------------------------
    // world work progress vocabulary
    // ------------------------------------------------------------------

    enum class EWorldJobState : u8 {
        idle,
        queued,
        generating,
        cancelling,
        prepared,
        retiring,
        succeeded,
        failed,
        cancelled
    };
    enum class EWorldJobStage : u8 {
        queued,
        layout,
        terrain,
        caves,
        initial_matter,
        validation,
        physics,
        publishing,
        retiring,
        capturing
    };

    struct WorldJobView {
        u64 m_id{};
        EWorldJobState m_state{EWorldJobState::idle};
        EWorldJobStage m_stage{EWorldJobStage::queued};
        u32 m_completed{};
        u32 m_total{}; // Zero denotes an indeterminate stage.
        std::error_code m_error{};
    };

}
