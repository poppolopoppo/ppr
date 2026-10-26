module;

export module game.application:session_service;

import engine.app;
import engine.core;
import engine.sim;
import game.colony.driver;
import game.colony.matter;
import game.colony.buildings;
import game.colony.digtool;
import game.colony.panel;
import game.colony.worldgen_panel;
import game.ui;
import std;

export namespace pP::game {
    // ------------------------------------------------------------------
    // activated-world presentation acknowledgment
    // ------------------------------------------------------------------

    /// Presentation proves visibility only; it never consumes staged edits.
    [[nodiscard]] bool hasSessionWorldPresentation(
        const RenderFrameResult &result, u64 reservation_epoch, GridWorldIdentity requested, bool published,
        bool activated, std::optional<GridParticlePresentationIdentity> resident = std::nullopt) noexcept;

    /// Selects the final game lease only while Workshop retains that same bundle.
    /// Current epoch/layout validation remains at the renderer's existing boundary.
    [[nodiscard]] std::optional<GridParticleGpuView> selectWorkshopRetainedParticles(
        const std::optional<GridParticleGpuView> &held, GridWorldIdentity game_world, bool retain_content,
        bool workshop_open, bool preview_displayed, bool finishing, bool activation_pending) noexcept;

    // ------------------------------------------------------------------
    // main-owner world transition
    // ------------------------------------------------------------------

    struct SessionWorldFacts {
        GridWorldIdentity m_world{};
        bool m_published{}, m_activated{}, m_presented{}, m_ready{}, m_paused{};
    };

    enum class ESessionWorldPhase : u8 {
        idle,
        detaching,
        preparing,
        prepared,
        activating,
        attaching,
        cancelling,
        resuming,
        awaiting_presentation,
        restored,
        complete
    };

    enum class ESessionWorldStep : u8 {
        wait,
        activate,
        release_candidate,
        publish,
        restore,
        render_return,
        finish
    };

    /// Value-only main-lane facts; Runtime/WorldJob retain all resource owners.
    struct SessionWorldTransition {
        SessionWorldFacts m_previous{};
        std::optional<GridParticlePresentationIdentity> m_expected{};
        u64 m_reservation_epoch{};
        std::error_code m_error{};
        ESessionWorldPhase m_phase{};
        bool m_returning{};
    };

    /// The old view is invalid for selection from activation admission until
    /// the new Runtime attachment and host ownership transfer acknowledge.
    [[nodiscard]] bool canSelectSessionWorldView(const SessionWorldTransition &transition) noexcept;

    /// Rollback resume owns Driver service until its exact acknowledgement;
    /// ordinary update cannot service a logically detached durable driver.
    [[nodiscard]] bool sessionTransitionOwnsDriverService(const SessionWorldTransition &transition) noexcept;

    struct SessionWorldTransitionInput {
        GridWorldIdentity m_world{};
        const RenderFrameResult *m_frame{};
        bool m_cancel{}, m_renderer_idle{}, m_activation_admitted{}, m_activated{};
    };

    /// Runs the real snapshot/prepare/attach/resume prefix through the existing
    /// owned Runtime slot. Activation events are exact host-correlated ACKs.
    /// publish must transfer the acknowledged owner before reporting m_error.
    [[nodiscard]] Expected<ESessionWorldStep> advanceSessionWorldTransition(
        SessionWorldTransition &transition, colony::ColonyDriver *current, colony::ColonyDriver *candidate,
        colony::MatterRuntime &runtime, const SessionWorldTransitionInput &input);

    // ------------------------------------------------------------------
    // exclusive main-owner session delivery
    // ------------------------------------------------------------------

    struct SessionColonyIntent {
        u64 m_world{};
        colony::ColonyIntent m_intent{};
    };

    struct SessionBuildOrder {
        u64 m_world{};
        colony::Footprint m_footprint{};
        colony::EColonyTool m_tool{};
    };

    struct SessionDigOrder {
        u64 m_world{};
        colony::DigRect m_rect{};
    };

    struct SessionWorldAction {
        u64 m_job{};
        ui::EWorldAction m_action{};
    };

    using SessionDelivery = std::variant<
        RenderFrameResult, GridWorldResult, SessionColonyIntent, colony::WorkshopIntent, SessionWorldAction,
        ui::EUnsavedAction, SessionBuildOrder, SessionDigOrder>;
    using SessionToolOrder = std::variant<SessionBuildOrder, SessionDigOrder>;

    /// Stale displayed-world orders are expected absence. A busy live driver
    /// returns device_or_resource_busy so the host can retain the exact order.
    [[nodiscard]] Expected<std::optional<sim::Entity>> applySessionBuildOrder(
        colony::ColonyDriver &driver, const SessionBuildOrder &order, u64 displayed_world);
    [[nodiscard]] Expected<bool> applySessionDigOrder(
        colony::ColonyDriver &driver, const SessionDigOrder &order, u64 displayed_world);

    struct SessionServiceBatch {
        Array<SessionDelivery> m_deliveries{};
        // Native close is an unordered safe-point latch, not a FIFO control.
        bool m_close{};
    };

    /// Main owns the inbox. Only the native-close latch permits cross-thread
    /// posting. A detached batch remains exclusive through device publication
    /// and dependent Scene mutation; arrivals wait for the next service.
    class SessionService final {
        enum class ECloseState : u8 {
            idle,
            pending,
            stopped
        };

        Array<SessionDelivery> m_pending{};
        Array<SessionToolOrder> m_retries{};
        mutable std::atomic<ECloseState> m_close{ECloseState::idle};
        bool m_active{};
        bool m_controls_open{true};
        bool m_receiver_open{true};

    public:
        [[nodiscard]] std::error_code post(SessionDelivery delivery);

        /// Busy tools wait for a scheduled service. They do not make fresh
        /// pending() immediately due; new controls can still wake that service.
        [[nodiscard]] std::error_code deferTool(SessionToolOrder order);
        void requestClose() const noexcept;
        [[nodiscard]] bool takeClose() noexcept;
        [[nodiscard]] bool acknowledgmentsPending() const noexcept;
        [[nodiscard]] Array<SessionDelivery> takeAcknowledgments();

        /// Empty batches still acquire exclusivity. Reentry consumes nothing.
        [[nodiscard]] std::optional<SessionServiceBatch> beginService();
        void endService() noexcept;

        /// Cancel queued controls and reject new ones, retaining finite result
        /// acknowledgments until the live lane has been drained.
        void stopControls();
        [[nodiscard]] std::error_code close();

        [[nodiscard]] bool active() const noexcept;
        [[nodiscard]] bool pending() const noexcept;
        [[nodiscard]] bool retriesPending() const noexcept;
    };
}
