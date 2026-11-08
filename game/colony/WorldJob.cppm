module;

export module game.application:world_job;

export import game.colony.world_progress;

import engine.app;
import engine.core;
import engine.sim;
import game.colony.driver;
import game.colony.checkpoint;
import std;

export namespace pP::colony {
    // ------------------------------------------------------------------
    // job state
    // ------------------------------------------------------------------

    // ------------------------------------------------------------------
    // world job
    // ------------------------------------------------------------------

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
        WorldJob(ParallelScheduler &chunks, Prepare prepare);
        ~WorldJob() noexcept;
        WorldJob(const WorldJob &) = delete;
        WorldJob &operator=(const WorldJob &) = delete;

        /// An override owns one request's immutable authored inputs through worker retirement.
        /// Empty keeps the application's default preparation policy.
        [[nodiscard]] std::error_code request(ColonyDriverDesc request, Prepare prepare = {});
        /// Caller freezes at a completed tick. update() takes an exact demand
        /// snapshot and detaches on main before scheduling the capture worker.
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
        [[nodiscard]] std::error_code update(
            bool loading_presented, bool allow_cleanup = true, bool independent_work_admission = false);

        [[nodiscard]] WorldJobView view() const noexcept;
        [[nodiscard]] std::string_view diagnosticText() const noexcept;
        [[nodiscard]] bool busy() const noexcept;

        /// Resident replacement calls after activation ACK and Runtime attachment.
        /// Activation admission latches commit intent; late cancellation cannot
        /// discard the acknowledged owner before this exactly-once transfer.
        [[nodiscard]] std::unique_ptr<ColonyDriver> publish();
        /// Exclusive main access after CPU completion. A successful device
        /// publication must immediately transfer this candidate to the host;
        /// cancellation/cleanup may never send an attached driver to a worker.
        [[nodiscard]] ColonyDriver *preparedDriver() noexcept;
        /// Main holds the prepared CPU owner while device operations acknowledge.
        void holdPreparedDeviceOwner(bool hold) noexcept;
        /// Exclusive main-lane access only after preparation; the job retains
        /// ownership so cancellation/disposal releases large payloads off-main.
        [[nodiscard]] GridStagedFrame &presentation();

        /// Host commits once after graphics activation and Runtime publication
        /// acknowledge. The old detached CPU owner retires afterward; real
        /// resident presentation remains a separate readiness observation.
        void commitPublication();
        void discardPrepared(std::error_code error);
        /// Before commit, retire a rejected candidate. After commit, retain its
        /// presentation owner and return the original terminal error; the live
        /// host driver is never rolled back or discarded.
        [[nodiscard]] std::error_code presentationFailed(std::error_code error);

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
        /// Owned by pointer, not by value: a by-value member of this imported
        /// class crashes the MSVC front end while compiling this interface
        /// (C1001, function-signature.cpp:213). A pointer member is unaffected.
        std::unique_ptr<ParallelScheduler> m_coordinator{};
        std::shared_ptr<State> m_job{};
        std::future<std::error_code> m_task{};
        WorldJobView m_view{};
        std::string m_diagnostic_text{};
        u64 m_next_id{1u};
        bool m_cleanup_pending{};
        bool m_cancelled{};
        bool m_publication_committed{};
        bool m_device_owner_held{};
        [[nodiscard]] std::error_code admitCleanup_();
        [[nodiscard]] std::error_code updateCapture_(bool loading_presented);
    };

}
