module;
export module game.application:worldgen_workshop;

import engine.core;
import engine.app;
import engine.sim;
import game.colony.driver;
import game.colony.content;
import game.colony.content.authoring;
import game.colony.worldgen_preview;
import std;

export namespace pP::colony {
    // ------------------------------------------------------------------
    // owned authored game preparation
    // ------------------------------------------------------------------

    struct AuthoredGameSource {
        ContentAuthoringSnapshot m_snapshot;
        std::string m_profile;
    };

    /// Descriptor bytes come from the owned snapshot. Presentation sources may still require filesystem I/O.
    [[nodiscard]] Expected<std::unique_ptr<ColonyDriver>> prepareAuthoredGame(
        const AuthoredGameSource &source, const ColonyDriverDesc &request, const ITimerClock &clock,
        const sim::GenerationExecution &execution, config::Diagnostics &diagnostics);

    // ------------------------------------------------------------------
    // asynchronous authoring workshop
    // ------------------------------------------------------------------

    /// Main-lane authoring owner; worker requests contain owned immutable snapshots.
    class WorldgenWorkshop {
    public:
        // construction:
        explicit WorldgenWorkshop(
            ParallelScheduler &pool, const ITimerClock &clock, ParallelScheduler *generation_pool = nullptr);
        ~WorldgenWorkshop() noexcept;

        // queries:
        [[nodiscard]] ContentAuthoringSession *session() noexcept;
        [[nodiscard]] const WorldgenPreviewResult *preview() const noexcept;
        [[nodiscard]] std::shared_ptr<const WorldgenPreviewResult> previewHandle() const noexcept;
        [[nodiscard]] std::string_view status() const noexcept;
        [[nodiscard]] std::string_view diagnostics() const noexcept;
        [[nodiscard]] std::span<const config::Diagnostic> propertyDiagnostics() const noexcept;
        [[nodiscard]] bool draftInvalid() const noexcept;
        [[nodiscard]] bool busy() const noexcept;

        // operations:
        [[nodiscard]] std::error_code open(
            std::filesystem::path root, std::optional<u64> discard_revision = std::nullopt);
        [[nodiscard]] std::error_code open(
            std::span<const std::filesystem::path> roots, std::optional<u64> discard_revision = std::nullopt);
        [[nodiscard]] std::error_code validate();
        [[nodiscard]] std::error_code requestPreview(WorldgenPreviewSpec spec, EGridDiagnosticMode mode);
        void configureLivePreview(bool enabled, WorldgenPreviewSpec spec, EGridDiagnosticMode mode);
        [[nodiscard]] std::error_code save();
        [[nodiscard]] std::error_code exportPreview(std::filesystem::path path, WorldgenPreviewDisplay display);
        void cancel() noexcept;
        void close() noexcept;
        [[nodiscard]] std::error_code update();
        [[nodiscard]] std::unique_ptr<GridStagedFrame> takePresentation();
        [[nodiscard]] std::error_code shutdown();

    private:
        struct Request;
        struct State;

        // execution and owned results:
        ParallelScheduler &m_pool;
        const ITimerClock &m_clock;
        ParallelScheduler *m_generation_pool;
        std::shared_ptr<const WorldgenPreviewWorld> m_world_cache{};
        std::optional<WorldgenPreviewSpec> m_live_spec{};
        TimePoint m_live_due{};
        std::unique_ptr<ContentAuthoringSession> m_session{};
        std::shared_ptr<State> m_running{}, m_completed{}, m_last_preview{}, m_retired_preview{};
        std::unique_ptr<Request> m_replacement{};
        std::future<std::error_code> m_task{};

        // Only this external main-lane owner retains an unfinished asset request.
        // Worker State values never own its coordinator or destructor drain.
        std::optional<ContentPresentationRequest> m_images{};
        std::unique_ptr<GridStagedFrame> m_presentation{};
        std::string m_status{"Open an authored pack root to begin."}, m_diagnostics{};
        config::Diagnostics m_property_diagnostics{64u};
        std::filesystem::path m_diagnostic_root{};
        u64 m_diagnostic_revision{};
        std::filesystem::path m_invalid_draft_root{};
        u64 m_invalid_draft_revision{};
        u64 m_next_request{1u}, m_latest_request{};
        EGridDiagnosticMode m_live_mode{};
        bool m_live_enabled{}, m_live_pending{}, m_validation_pending{}, m_closing{};

        // admission:
        [[nodiscard]] std::error_code queue_(Request request);
        [[nodiscard]] bool explicitOperationPending_() const noexcept;
    };
}
