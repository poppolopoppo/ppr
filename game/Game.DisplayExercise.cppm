export module game.display_exercise;

// ------------------------------------------------------------------
// display exercise
// ------------------------------------------------------------------

import engine.app;
import engine.core;
import engine.math;
import game.config;
import game.display;

import std;

export namespace pP::game {
    // ------------------------------------------------------------------
    // exercise vocabulary
    // ------------------------------------------------------------------

    /// Per-transition automation deadline after readiness carries no failure
    /// semantics of its own: a miss preserves state and reports incomplete.
    inline constexpr float kDisplayExerciseTransitionSeconds{10.0f};

    /// Initial world admission and first presented frame. Bounds automation,
    /// never accepted game performance.
    inline constexpr float kDisplayExerciseInitialLoadSeconds{180.0f};

    /// Whole-run ceiling. A miss preserves evidence and attempts ordinary quit.
    inline constexpr float kDisplayExerciseRunSeconds{900.0f};

    /// Keep/Revert rollback deadline: the production confirmation duration
    /// plus ten seconds. The timeout case waits for the real production timer.
    inline constexpr float kDisplayExerciseConfirmRollbackSeconds{kDisplayConfirmSeconds + 10.0f};

    /// Minimized suspension observation window; no fresh presented frame may
    /// arrive while the real window stays iconified.
    inline constexpr float kDisplayExerciseSuspensionSeconds{0.5f};

    enum class EDisplayExerciseStatus : u8 {
        pending,
        passed,
        failed,
        unsupported,
        incomplete,
    };

    [[nodiscard]] std::string_view displayExerciseStatusName(EDisplayExerciseStatus status) noexcept;

    /// Exit mapping: 0=passed, 1=failed, 2=invalid invocation (outside this
    /// driver), 3=incomplete-or-unsupported (not a pass).
    [[nodiscard]] int displayExerciseExitCode(EDisplayExerciseStatus status) noexcept;

    /// Typed production intent. Every display/scene change rides the exact
    /// panel/window path: the host installs the draft and dispatches the same
    /// `ColonyIntent` the panel emits, or performs the ordinary window-service
    /// iconify/restore. Nothing here is a private renderer setter.
    enum class EDisplayExerciseActionKind : u8 {
        apply_display,
        keep_display,
        revert_display,
        reset_display,
        save_display,
        live_picture,
        reset_live_picture,
        restore_live_baseline,
        invalid_draft,
        minimize_window,
        restore_window,
        reload_overlay,
        capture_image,
        wait_suspension,
        quit,
    };

    struct DisplayExerciseAction final {
        EDisplayExerciseActionKind m_kind{};
        DisplayDraft m_draft{};
        WorldLightDisplaySettings m_world_light{};
        PixelDisplaySettings m_pixel{};
        PostDisplaySettings m_post{};
        std::string m_case{};
        std::string m_image{};
    };

    /// Copied value-only observation supplied by the host each main turn. It
    /// deliberately carries no window/pass pointer or borrowed storage: no
    /// callback borrowing the observation can escape the advance call.
    struct DisplayExerciseObservation final {
        // capability snapshot (host-refreshed, owned copy):
        DisplayCapabilities m_capabilities{};
        bool m_capabilities_available{};
        std::error_code m_capabilities_error{};

        // display transaction / acknowledged configuration:
        DisplaySettings m_committed{};
        u64 m_settings_revision{};
        u64 m_surface_generation{};
        bool m_transaction_pending{};
        /// False while the host still owns an unprocessed display watch. The
        /// acknowledgement is only actionable after the production poll has
        /// consumed it (and armed the Keep/Revert trial when applicable).
        bool m_ack_settled{true};
        bool m_confirm_armed{};
        float m_confirm_seconds_left{};

        // last dispatched action's immediate result (host-owned):
        bool m_action_submitted{};
        std::error_code m_action_error{};
        std::string m_action_message{};

        // observed native geometry (logical vs framebuffer kept distinct):
        int2 m_logical_extent{zero_v};
        int2 m_framebuffer_extent{zero_v};
        bool m_fullscreen{};
        std::string m_monitor{};
        std::optional<u32> m_refresh_hz{};
        bool m_focused{true};
        bool m_iconified{};
        u64 m_focus_losses{};
        u64 m_iconify_events{};
        bool m_background_priority{};
        bool m_throttle_active{};

        // presentation identity:
        bool m_world_ready{};
        u64 m_presented_frame{};
        u64 m_presented_generation{};
        GridWorldIdentity m_presented_world{};
        bool m_presented_inspection{};
        LightingInspectionSettings m_presented_inspection_settings{};
        u64 m_live_revision{};
        u64 m_presented_live_revision{};
        bool m_history_available{};
        bool m_history_established{};
        u64 m_history_reset_count{};
        ReconstructionHistoryStamp m_history_last_reset{};

        // live session values (the same values the Display window edits):
        WorldLightDisplaySettings m_live_world_light{};
        PixelDisplaySettings m_live_pixel{};
        PostDisplaySettings m_live_post{};

        // artifact results:
        bool m_capture_complete{};
        std::error_code m_capture_error{};
        std::string m_capture_path{};
        bool m_save_complete{};
        std::error_code m_save_error{};
        bool m_reload_complete{};
        std::error_code m_reload_error{};
        DisplaySettings m_reload_display{};
        WorldLightDisplaySettings m_reload_world_light{};
        PixelDisplaySettings m_reload_pixel{};
        PostDisplaySettings m_reload_post{};

        // shutdown/interruption:
        bool m_close_requested{};
        std::error_code m_shutdown_error{};

        // startup baseline, reported for reset/revert comparisons:
        DisplaySettings m_startup_display{};
        WorldLightDisplaySettings m_startup_world_light{};
        PixelDisplaySettings m_startup_pixel{};
        PostDisplaySettings m_startup_post{};
        Array<std::string> m_cli_assignments{};

        // host identity:
        std::string m_backend{};
        std::string m_gpu{};
        std::string m_executable_path{};
        u64 m_executable_bytes{};
    };

    /// One reported coverage case. The driver owns the strings; callers
    /// borrow them only for the duration of the report/iteration call.
    struct DisplayExerciseCase final {
        std::string m_id{};
        std::string m_group{};
        std::string m_description{};
        EDisplayExerciseStatus m_status{EDisplayExerciseStatus::pending};
        std::string m_reason{};
        std::string m_action{};
        std::string m_requested{};
        u32 m_requested_width{};
        u32 m_requested_height{};
        int2 m_observed_logical{zero_v};
        int2 m_observed_framebuffer{zero_v};
        int2 m_final_logical{zero_v};
        int2 m_final_framebuffer{zero_v};
        bool m_fullscreen{};
        std::string m_monitor{};
        std::optional<u32> m_refresh_hz{};
        PresentationGrid m_presentation_grid{};
        bool m_has_presentation_grid{};
        PresentationGrid m_history_grid{};
        bool m_has_history_grid{};
        bool m_history_reset{};
        u64 m_history_reset_frame{};
        u32 m_history_index{};
        u32 m_history_phase{};
        bool m_trial_armed{};
        bool m_trial_kept{};
        float m_trial_seconds_left{};
        u64 m_ack_revision{};
        u64 m_presented_frame{};
        double m_start_seconds{};
        double m_end_seconds{};
        std::string m_image{};
    };

    /// Test-only scripted step. The permanent plan is built by begin(); this
    /// seam lets focused unit tests inject one production action per step
    /// without reproducing the entire nine-group matrix.
    struct DisplayExerciseTestStep final {
        EDisplayExerciseActionKind m_kind{};
        std::string m_case{};
        DisplayDraft m_draft{};
        WorldLightDisplaySettings m_world_light{};
        PixelDisplaySettings m_pixel{};
        PostDisplaySettings m_post{};
        bool m_completes_case{true};
        bool m_expect_rejection{};
        bool m_keep_after_ack{};
        bool m_revert_after_ack{};
        bool m_timeout_after_ack{};
        bool m_require_inspection_stamp{};
        bool m_require_unchanged_world{};
        bool m_accept_clamp{};
        bool m_require_trial_armed{};
    };

    // ------------------------------------------------------------------
    // display exercise driver
    // ------------------------------------------------------------------

    /// Game-owned asynchronous state machine advanced by production action
    /// results, real window events and injected deadlines. It emits at most
    /// one action per advance and never overlaps an outstanding native trial.
    class DisplayExercise final {
    public:
        DisplayExercise() = default;

        /// Validates the output directory, creates it, builds the permanent
        /// nine-group plan and writes the initial atomic schema-1 report.
        [[nodiscard]] std::error_code begin(const std::filesystem::path &output, TimePoint now);

        /// Test-only scripted plan with the same state-machine semantics.
        [[nodiscard]] std::error_code beginForTest(
            const std::filesystem::path &output, TimePoint now, std::span<const DisplayExerciseTestStep> steps);

        [[nodiscard]] Expected<std::optional<DisplayExerciseAction>> advance(
            const DisplayExerciseObservation &observed, TimePoint now);

        /// Records the shutdown outcome, marks unreached cases incomplete and
        /// persists the final atomic report. Returns the exit-mapped status as
        /// an error code (success only for a genuine pass).
        [[nodiscard]] std::error_code finish(std::error_code cleanup_error);

        [[nodiscard]] EDisplayExerciseStatus status() const noexcept {
            return m_status;
        }

        [[nodiscard]] std::span<const DisplayExerciseCase> cases() const noexcept {
            return {m_cases.data(), m_cases.size()};
        }

        [[nodiscard]] const std::filesystem::path &output() const noexcept {
            return m_output;
        }

        /// True only if a mismatched/stale acknowledgement ever completed a
        /// case. The permanent driver always keeps this false.
        [[nodiscard]] bool staleAckAdvanced() const noexcept {
            return m_stale_ack_advanced;
        }

        /// Actions emitted after a real user close was observed. Must be zero:
        /// a closed run starts no new trial and preserves cleanup evidence.
        [[nodiscard]] u64 emittedActionsAfterClose() const noexcept {
            return m_emitted_after_close;
        }

        [[nodiscard]] u64 emittedActions() const noexcept {
            return m_emitted_actions;
        }

        /// True once a matching acknowledgement armed the production trial.
        [[nodiscard]] bool confirmationStarted() const noexcept {
            return m_confirmation_started;
        }

        [[nodiscard]] bool needsCapabilities() const noexcept {
            return m_need_capabilities;
        }

    private:
        // ------------------------------------------------------------------
        // scripted plan
        // ------------------------------------------------------------------

        struct Step final {
            EDisplayExerciseActionKind m_kind{};
            std::string m_case{};
            std::string m_image{};
            DisplayDraft m_draft{};
            WorldLightDisplaySettings m_world_light{};
            PixelDisplaySettings m_pixel{};
            PostDisplaySettings m_post{};
            bool m_completes_case{true};
            bool m_expect_rejection{};
            bool m_keep_after_ack{};
            bool m_revert_after_ack{};
            bool m_timeout_after_ack{};
            bool m_require_inspection_stamp{};
            bool m_require_unchanged_world{};
            bool m_accept_clamp{};
            bool m_require_trial_armed{};
            bool m_require_minimized{};
            bool m_record_as_tested{true};
            bool m_keep_world_light{};
            bool m_keep_pixel{};
            bool m_keep_post{};
            bool m_expand_fullscreen{};
        };

        enum class EPhase : u8 {
            idle,
            loading,
            running,
            awaiting,
            draining,
            stopping,
            finished,
        };

        void buildPermanentPlan_();
        void addCase_(std::string id, std::string group, std::string description);
        void addStep_(Step step);
        void addApplyKeep_(
            std::string case_id, DisplayDraft draft, bool accept_clamp = false, bool completes_case = true);
        void addApplyThen_(std::string case_id, DisplayDraft draft, bool revert, bool timeout);
        void addWindowed_(
            std::string case_id, u32 width, u32 height, bool accept_clamp = false, bool completes_case = true);
        void addLive_(
            std::string case_id, bool keep_world_light, WorldLightDisplaySettings world_light, bool keep_pixel,
            PixelDisplaySettings pixel, bool keep_post, PostDisplaySettings post, bool require_inspection_stamp = false,
            bool completes_case = true);
        void addCapture_(std::string case_id, std::string image);
        void addFullscreenGroup_();
        void expandFullscreen_(const DisplayExerciseObservation &observed);

        // ------------------------------------------------------------------
        // state machine
        // ------------------------------------------------------------------

        [[nodiscard]] Expected<std::optional<DisplayExerciseAction>> run_(
            const DisplayExerciseObservation &observed, TimePoint now);
        [[nodiscard]] std::optional<DisplayExerciseAction> emitStep_(
            Step &step, const DisplayExerciseObservation &observed, TimePoint now);
        [[nodiscard]] std::optional<DisplayExerciseAction> publish_(DisplayExerciseAction action);
        [[nodiscard]] bool gateStep_(Step &step, const DisplayExerciseObservation &observed);
        [[nodiscard]] bool pollAwaiting_(const DisplayExerciseObservation &observed, TimePoint now);
        void captureBaseline_(const DisplayExerciseObservation &observed, TimePoint now);
        void onUserClose_(const DisplayExerciseObservation &observed, TimePoint now);
        void markUnreached_(std::string_view reason);
        void beginStopping_(std::string_view reason);
        void completeStep_(
            Step &step, EDisplayExerciseStatus status, std::string reason, const DisplayExerciseObservation &observed,
            TimePoint now);
        void markCase_(std::string_view id, EDisplayExerciseStatus status, std::string reason);
        [[nodiscard]] bool awaitMatchesDraft_(const Step &step, const DisplayExerciseObservation &observed) const;
        [[nodiscard]] bool requestsWindowChange_(
            const DisplaySettings &candidate, const DisplaySettings &committed,
            const DisplayCapabilities &capabilities) const;
        [[nodiscard]] bool awaitMatchesPrior_(const DisplayExerciseObservation &observed) const;
        [[nodiscard]] DisplayExerciseCase *findCase_(std::string_view id) noexcept;
        [[nodiscard]] bool cliOverrides_(std::string_view property) const;
        void computeFinalStatus_();

        // ------------------------------------------------------------------
        // artifacts
        // ------------------------------------------------------------------

        [[nodiscard]] std::error_code writeReport_();
        void writeLog_(std::string_view line);
        [[nodiscard]] std::string reportJson_() const;
        [[nodiscard]] static std::string jsonString_(std::string_view value);
        [[nodiscard]] static std::string jsonVector_(const int2 &value);
        [[nodiscard]] static std::string jsonGrid_(const PresentationGrid &grid);
        [[nodiscard]] static std::string statusText_(EDisplayExerciseStatus status);
        [[nodiscard]] static std::string_view actionName_(EDisplayExerciseActionKind kind) noexcept;
        [[nodiscard]] std::string describeRequested_(
            const Step &step, const DisplayExerciseObservation &observed) const;

        // ------------------------------------------------------------------
        // state
        // ------------------------------------------------------------------

        std::filesystem::path m_output{};
        std::filesystem::path m_log_path{};
        std::filesystem::path m_report_path{};
        Array<Step> m_steps{};
        Array<DisplayExerciseCase> m_cases{};
        std::size_t m_next_step{};
        std::size_t m_fullscreen_case_insert{};
        std::optional<std::size_t> m_awaiting_step{};

        EPhase m_phase{EPhase::idle};
        EDisplayExerciseStatus m_status{EDisplayExerciseStatus::pending};
        TimePoint m_begin{}, m_last_now{};
        TimePoint m_await_start{}, m_await_deadline{};
        TimePoint m_ack_time{}, m_last_wait_log{};
        std::string m_stop_reason{};

        bool m_quit_emitted{};
        bool m_quit_confirmed{};
        bool m_user_close{};
        bool m_actions_suppressed{};
        u64 m_emitted_actions{};
        u64 m_emitted_after_close{};
        bool m_stale_ack_advanced{};
        bool m_pending_stale_ack{};
        bool m_confirmation_started{};
        bool m_cleanup_keep_sent{};
        bool m_cleanup_pending{};
        bool m_need_capabilities{true};
        u32 m_capability_waits{};
        DisplayCapabilities m_capabilities{};
        bool m_capabilities_available{};
        std::error_code m_capabilities_error{};

        // baselines:
        DisplaySettings m_baseline_display{};
        WorldLightDisplaySettings m_baseline_world_light{};
        PixelDisplaySettings m_baseline_pixel{};
        PostDisplaySettings m_baseline_post{};
        GridWorldIdentity m_baseline_world{};
        PresentationGrid m_baseline_grid{};
        int2 m_baseline_logical{zero_v};
        int2 m_baseline_framebuffer{zero_v};
        u64 m_baseline_revision{};
        u64 m_baseline_frame{};
        bool m_baseline_recorded{};

        // awaiting-action context:
        DisplaySettings m_await_prior{};
        DisplaySettings m_last_apply_prior{};
        u64 m_await_revision{};
        u64 m_await_frame{};
        u64 m_await_live_revision{};
        u64 m_await_iconify_events{};
        u64 m_await_history_reset_count{};
        PixelDisplaySettings m_await_pixel{};
        int2 m_await_logical{zero_v};
        bool m_await_confirmed{};
        bool m_await_window_change{};
        bool m_last_apply_window_change{};
        u64 m_timeout_ack_revision{};
        u64 m_minimize_iconify_events{};

        // last saved values for the reload comparison:
        DisplaySettings m_saved_display{};
        WorldLightDisplaySettings m_saved_world_light{};
        PixelDisplaySettings m_saved_pixel{};
        PostDisplaySettings m_saved_post{};

        // report identity:
        std::string m_backend{};
        std::string m_gpu{};
        std::string m_executable_path{};
        u64 m_executable_bytes{};
        Array<std::string> m_cli_assignments{};

        std::error_code m_cleanup_error{};
        bool m_finalized{};
    };
} // namespace pP::game
