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

    public:
        /// Drop the old world's staged tiles and cached cell materials before
        /// submitting a regenerated grid (or after replacing the GridPass).
        void reset(GridPass &pass) noexcept;

        /// Invalidate a resident chunk after an edit. The caller must notify
        /// this translator for subsequent edits; sim dirty flags are not used.
        [[nodiscard]] std::error_code markChunkChanged(sim::ChunkPos pos) noexcept;

        /// Stage every resident chunk each frame; upload only unseen/changed
        /// chunks. An error clears pass staging/cache and invalidates all
        /// presentation revisions so the next call retries the entire world.
        [[nodiscard]] std::error_code submit(const sim::ChunkGrid &grid, GridPass &pass);

        /// Submitted resident chunks (not camera-frustum visibility).
        [[nodiscard]] u32 submittedChunks() const noexcept;

        [[nodiscard]] static constexpr u32 totalChunks() noexcept { return sim::kChunkCount; }
    };
}
