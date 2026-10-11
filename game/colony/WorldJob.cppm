module;

export module game.application:world_job;

import engine.app;
import engine.core;
import engine.sim;
import game.colony.driver;
import game.colony.checkpoint;
import std;

export namespace pP::colony {
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

    /// One game-owned coordinator job. Requests/candidates never borrow live
    /// session state. The owner polls from its normal loop and must cancel,
    /// drain and retire before destruction; ordinary close does this without
    /// blocking the UI. Final join is only teardown/fatal-path cleanup.
    class WorldJob final {
    public:
        using Prepare = std23::move_only_function<Expected<std::unique_ptr<ColonyDriver>>(
            const ColonyDriverDesc &, const sim::GenerationExecution &, config::Diagnostics &)>;
        using Capture = std23::move_only_function<std::error_code(const ColonyDriver &, const IContext &)>;
        /// Explicit legacy smoke/many-agent fixture; normal sessions supply authored preparation.
        [[nodiscard]] static Prepare legacyFixturePrepare();
        WorldJob(ParallelScheduler &chunks, Prepare prepare);
        ~WorldJob() noexcept;
        WorldJob(const WorldJob &) = delete;
        WorldJob &operator=(const WorldJob &) = delete;

        [[nodiscard]] std::error_code request(ColonyDriverDesc request);
        /// Caller freezes at a completed tick and mirrors Scene state first.
        /// Transfers ownership only on success. Until takeCaptured(), all live
        /// inspection, ticks, tools and render acknowledgments must be deferred.
        [[nodiscard]] std::error_code requestCapture(std::unique_ptr<ColonyDriver> &frozen, Capture capture);
        [[nodiscard]] std::error_code requestCheckpoint(std::unique_ptr<ColonyDriver> &frozen);
        /// Move the owned result before releasing capture access. Encoding/I/O
        /// must continue off-main using this value, never the live session.
        [[nodiscard]] WorldCheckpoint takeCheckpoint();
        [[nodiscard]] std::error_code requestPreview(
            std::unique_ptr<ColonyDriver> &frozen, EGridDiagnosticMode mode, float temperature_min_k,
            float temperature_max_k);
        /// Returns the frozen owner after success, failure or cancellation.
        /// The caller restores pause state and neutral input before resuming.
        [[nodiscard]] std::unique_ptr<ColonyDriver> takeCaptured(bool retain_presentation = false);
        void cancel() noexcept;
        /// Non-waiting admission and readiness polling. Initial admission is
        /// withheld until a successful loading-screen presentation is observed.
        /// Hold cleanup while an external header command still shares catalog
        /// image storage. Release the command before allowing worker retirement.
        [[nodiscard]] std::error_code update(bool loading_presented, bool allow_cleanup = true);
        [[nodiscard]] WorldJobView view() const noexcept;
        [[nodiscard]] std::string_view diagnosticText() const noexcept;
        [[nodiscard]] bool busy() const noexcept;
        /// Call only in prepared state, after the final cancellation check.
        [[nodiscard]] std::unique_ptr<ColonyDriver> publish();
        /// Exclusive main-lane access only after preparation; the job retains
        /// ownership so cancellation/disposal releases large payloads off-main.
        [[nodiscard]] GridPass::GridStagedFrame &presentation();
        /// The accepted GPU activation makes publication irrevocable.
        void commitPublication();
        void discardPrepared(std::error_code error);
        /// Transfers the detached old session for coordinator-side disposal.
        /// It remains retained on queue-full and admission retries in update.
        void retire(std::unique_ptr<ColonyDriver> old);
        /// Standalone final session disposal, through the same bounded job.
        void dispose(std::unique_ptr<ColonyDriver> old);
        [[nodiscard]] std::error_code shutdown();

    private:
        struct State;
        ParallelScheduler &m_chunks;
        Prepare m_prepare;
        ParallelScheduler m_coordinator;
        std::shared_ptr<State> m_job{};
        std::future<std::error_code> m_task{};
        WorldJobView m_view{};
        std::string m_diagnostic_text{};
        u64 m_next_id{1u};
        bool m_cleanup_pending{};
        bool m_cancelled{};
        bool m_publication_committed{};
        [[nodiscard]] std::error_code admitCleanup_();
        [[nodiscard]] std::error_code updateCapture_(bool loading_presented);
    };

}
