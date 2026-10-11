module;

export module game.application:world_job;

import engine.core;
import engine.sim;
import game.colony.driver;
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
        retiring
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
            const ColonyDriverDesc &, const sim::GenerationExecution &)>;
        explicit WorldJob(ParallelScheduler &chunks);
        WorldJob(ParallelScheduler &chunks, Prepare prepare);
        ~WorldJob() noexcept;
        WorldJob(const WorldJob &) = delete;
        WorldJob &operator=(const WorldJob &) = delete;

        [[nodiscard]] std::error_code request(ColonyDriverDesc request);
        void cancel() noexcept;
        /// Non-waiting admission and readiness polling. Initial admission is
        /// withheld until a successful loading-screen presentation is observed.
        [[nodiscard]] std::error_code update(bool loading_presented);
        [[nodiscard]] WorldJobView view() const noexcept;
        [[nodiscard]] bool busy() const noexcept;
        /// Call only in prepared state, after the final cancellation check.
        [[nodiscard]] std::unique_ptr<ColonyDriver> publish();
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
        u64 m_next_id{1u};
        bool m_cleanup_pending{};
        bool m_cancelled{};
        [[nodiscard]] std::error_code admitCleanup_();
    };

}
