module;
export module game.application:worldgen_workshop;
import engine.core;
import engine.app;
import game.colony.content;
import game.colony.content.authoring;
import game.colony.worldgen_preview;
import std;

export namespace pP::colony {
    /// Main-lane authoring owner; worker requests contain owned immutable snapshots.
    class WorldgenWorkshop {
    public:
        explicit WorldgenWorkshop(
            ParallelScheduler &pool, const ITimerClock &clock, ParallelScheduler *generation_pool = nullptr);
        ~WorldgenWorkshop() noexcept;
        [[nodiscard]] ContentAuthoringSession *session() noexcept;
        [[nodiscard]] const WorldgenPreviewResult *preview() const noexcept;
        [[nodiscard]] std::shared_ptr<const WorldgenPreviewResult> previewHandle() const noexcept;
        [[nodiscard]] std::string_view status() const noexcept;
        [[nodiscard]] std::string_view diagnostics() const noexcept;
        [[nodiscard]] std::span<const config::Diagnostic> propertyDiagnostics() const noexcept;
        [[nodiscard]] bool draftInvalid() const noexcept;
        [[nodiscard]] bool busy() const noexcept;
        [[nodiscard]] std::error_code open(
            std::filesystem::path root, std::optional<u64> discard_revision = std::nullopt);
        [[nodiscard]] std::error_code validate();
        [[nodiscard]] std::error_code requestPreview(WorldgenPreviewSpec spec, EGridDiagnosticMode mode);
        void configureLivePreview(bool enabled, WorldgenPreviewSpec spec, EGridDiagnosticMode mode);
        [[nodiscard]] std::error_code save();
        [[nodiscard]] std::error_code exportPreview(std::filesystem::path path, WorldgenPreviewDisplay display);
        void cancel() noexcept;
        void close() noexcept;
        [[nodiscard]] std::error_code update();
        [[nodiscard]] std::unique_ptr<GridPass::GridStagedFrame> takePresentation();
        [[nodiscard]] std::error_code shutdown();

    private:
        struct Request;
        struct State;
        ParallelScheduler &m_pool;
        const ITimerClock &m_clock;
        ParallelScheduler *m_generation_pool;
        std::shared_ptr<const WorldgenPreviewWorld> m_world_cache{};
        std::optional<WorldgenPreviewSpec> m_live_spec{};
        EGridDiagnosticMode m_live_mode{};
        TimePoint m_live_due{};
        bool m_live_enabled{}, m_live_pending{}, m_validation_pending{};
        std::unique_ptr<ContentAuthoringSession> m_session{};
        std::shared_ptr<State> m_running{}, m_completed{}, m_last_preview{}, m_retired_preview{};
        std::unique_ptr<Request> m_replacement{};
        std::future<std::error_code> m_task{};
        std::unique_ptr<GridPass::GridStagedFrame> m_presentation{};
        std::string m_status{"Open an authored pack root to begin."}, m_diagnostics{};
        config::Diagnostics m_property_diagnostics{64u};
        std::filesystem::path m_diagnostic_root{};
        u64 m_diagnostic_revision{};
        std::filesystem::path m_invalid_draft_root{};
        u64 m_invalid_draft_revision{};
        u64 m_next_request{1u}, m_latest_request{};
        bool m_closing{};
        [[nodiscard]] std::error_code queue_(Request request);
        [[nodiscard]] bool explicitOperationPending_() const noexcept;
    };
}
