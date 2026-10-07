module;
#include "StageTiming.h"

export module game.colony.translator;

import engine.app;
import engine.core;
import engine.sim;
import std;

export namespace pP::colony {
    /// Render-thread confined presentation state for one grid/pass pair. The
    /// grid's snapshot dirty flags are independent of these upload revisions.
    class ColonyTranslator {
    private:
        std::array<bool, sim::kChunkCount> m_presented{};
        u32 m_submitted_chunks{};
        StageTimings m_timings{};

    public:
        /// Drop the old world's staged tiles and cached cell materials before
        /// submitting a regenerated grid (or after replacing the GridPass).
        void reset(GridPass &pass) noexcept;

        /// Invalidate a resident chunk after an edit. The caller must notify
        /// this translator for subsequent edits; sim dirty flags are not used.
        [[nodiscard]] std::error_code markChunkChanged(sim::ChunkPos pos) noexcept;

        // Main-side state only: drops the per-chunk presentation
        // revisions and the submitted-chunk count. Does NOT touch any
        // GridPass state — the pass clearing that used to ride along in
        // reset(pass) now rides the staged payload's m_reset_caches onto
        // the lane at the next staged apply (see GridStagedFrame).
        void invalidate() noexcept;

        /// Stage every resident chunk each frame; upload only unseen/changed
        /// chunks. An error clears pass staging/cache and invalidates all
        /// presentation revisions so the next call retries the entire world.
        [[nodiscard]] std::error_code submit(const sim::ChunkGrid &grid, GridPass &pass);

        /// Submitted resident chunks (not camera-frustum visibility).
        [[nodiscard]] u32 submittedChunks() const noexcept;

        /// Accumulated submit-side wall-clock timings (observation only).
        /// Owned separately from the driver's in-tick table: submit runs on
        /// the presentation boundary with its own lifetime, so the driver
        /// never merges this table — both are reported side by side.
        [[nodiscard]] const StageTimings &stageTimings() const noexcept;

        /// Clears the accumulated submit timings.
        void resetStageTimings() noexcept;

        [[nodiscard]] static constexpr u32 totalChunks() noexcept {
            return sim::kChunkCount;
        }
    };
}
