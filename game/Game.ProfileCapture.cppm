export module game.profile_capture;

// ------------------------------------------------------------------
// profile capture
// ------------------------------------------------------------------

import engine.app;
import engine.core;
import engine.math;
import game.config;

import std;

export namespace pP::game {
    // ------------------------------------------------------------------
    // capture vocabulary
    // ------------------------------------------------------------------

    /// Readiness is warm-up evidence, not a claim that lighting converged.
    inline constexpr u32 kProfileCaptureReadyFrames{120u};
    inline constexpr float kProfileCaptureReadyActiveSeconds{2.0f};

    /// Whole-run readiness ceiling. A miss is an incomplete capture with
    /// retained logs; it never becomes a pass.
    inline constexpr float kProfileCaptureReadyDeadlineSeconds{180.0f};

    /// Intended collection interval requested by the maintained command.
    inline constexpr float kProfileCaptureCollectSeconds{2.0f};

    /// Tool-bounded Graphics interval; fewer complete frames are reported as
    /// their actual count rather than assumed.
    inline constexpr u32 kProfileCaptureGraphicsFrames{8u};
    inline constexpr float kProfileCaptureGraphicsMaxMilliseconds{2000.0f};

    inline constexpr float kProfileCaptureFinalizeDeadlineSeconds{120.0f};
    inline constexpr float kProfileCaptureTotalDeadlineSeconds{480.0f};

    /// Session rendezvous confined to the maintained profiling command: the
    /// supervisor writes this file with the session token, and only a matching
    /// token can request the ordinary quit.
    inline constexpr std::string_view kProfileCaptureStopRequestFileName{"stop.request"};
    inline constexpr std::string_view kProfileCaptureReadyFileName{"ready.json"};
    inline constexpr std::string_view kProfileCaptureFinalStateFileName{"final-state.json"};
    inline constexpr std::string_view kProfileCaptureLogFileName{"profile-capture.log"};

    /// The application owns readiness/final-state records so readiness identity
    /// never depends on wall-clock guessing in the supervisor.
    inline constexpr u32 kProfileCaptureSchemaVersion{1u};

    enum class EProfileCaptureTool : u8 {
        systems,
        graphics,
    };

    enum class EProfileCaptureWorkload : u8 {
        frozen,
        live,
    };

    enum class EProfileCaptureStatus : u8 {
        pending,
        ready,
        complete,
        incomplete,
        failed,
        unsupported,
    };

    [[nodiscard]] std::string_view profileCaptureToolName(EProfileCaptureTool tool) noexcept;

    [[nodiscard]] std::string_view profileCaptureWorkloadName(EProfileCaptureWorkload workload) noexcept;

    [[nodiscard]] std::string_view profileCaptureStatusName(EProfileCaptureStatus status) noexcept;

    enum class EProfileCaptureActionKind : u8 {
        pause_simulation,
        sdk_operation,
        quit,
    };

    /// Requested vendor-SDK operations. The host executes each one through the
    /// private lane-bound adapter; no vendor header or type crosses this
    /// interface, and normal gameplay never constructs an adapter.
    enum class EProfileCaptureSdkOperation : u8 {
        none,
        initialize,
        activate,
        start_trace,
        poll,
    };

    struct ProfileCaptureAction final {
        EProfileCaptureActionKind m_kind{};
        EProfileCaptureSdkOperation m_operation{};
    };

    /// Value-only vendor state copied into the observation. The numeric status
    /// is the adapter's own mapping of the vendor enum, so the driver can
    /// distinguish active/tracing/draining/errored without importing it.
    struct ProfileCaptureSdkStatus final {
        bool m_injected{};
        bool m_initialized{};
        bool m_active{};
        bool m_tracing{};
        bool m_draining{};
        bool m_errored{};
        u32 m_status{};
        u32 m_trace_files{};
        std::string m_trace_path{};
    };

    /// Profiling-only vendor seam. Implemented by the private adapter
    /// (`Game.ProfileCapture.Nsight.cpp`); the driver owns no vendor type.
    class IProfileCaptureSdk {
    public:
        IProfileCaptureSdk() = default;

        IProfileCaptureSdk(const IProfileCaptureSdk &) = delete;
        IProfileCaptureSdk &operator=(const IProfileCaptureSdk &) = delete;
        IProfileCaptureSdk(IProfileCaptureSdk &&) = delete;
        IProfileCaptureSdk &operator=(IProfileCaptureSdk &&) = delete;

        virtual ~IProfileCaptureSdk() = default;

        /// False when the profiling build has no SDK or the installed tool did
        /// not inject this process. A requested Graphics capture then fails
        /// explicitly instead of substituting another collection path.
        [[nodiscard]] virtual bool available() const noexcept = 0;

        [[nodiscard]] virtual std::error_code initialize() = 0;

        /// Device-owner-lane activation with the backend queue object obtained
        /// from the existing RHI native-object seam.
        [[nodiscard]] virtual std::error_code activate(void *native_queue) = 0;

        [[nodiscard]] virtual std::error_code startTrace() = 0;

        /// Nonblocking: never waits for a trace file or the render loop.
        [[nodiscard]] virtual std::error_code poll(ProfileCaptureSdkStatus &out_status) = 0;
    };

    /// Returns the vendor adapter when the profiling build configured one,
    /// otherwise a null pointer. Never initializes or calls a vendor API.
    [[nodiscard]] std::unique_ptr<IProfileCaptureSdk> createProfileCaptureSdk();

    struct ProfileCaptureSetting final {
        std::string m_name{};
        std::string m_value{};
    };

    /// Copied value-only observation supplied by the host each main turn. It
    /// carries per-advance presentation accounting, identity/settings/camera,
    /// capture finalization and shutdown state, and never a window/pass pointer.
    struct ProfileCaptureObservation final {
        // accepted presentation accounting since the previous advance:
        u32 m_accepted_scene_frames{};
        u32 m_ui_only_frames{};
        u32 m_rejected_frames{};
        double m_active_presentation_seconds{};

        // world/publication identity and accepted indices:
        bool m_world_admitted{};
        bool m_simulation_paused{};
        u64 m_tick{};
        u64 m_scene_frame_index{};
        u64 m_screen_frame_index{};
        u64 m_world_id{};
        u64 m_publication_id{};

        // actual camera and presented extents:
        float3 m_camera_origin{zero_v};
        float3 m_camera_forward{zero_v};
        int2 m_logical_extent{zero_v};
        int2 m_framebuffer_extent{zero_v};

        // explicit profiling settings, already isolated from user persistence:
        Array<ProfileCaptureSetting> m_settings{};

        // result of the lane-bound vendor operation finished since the last
        // advance (only meaningful when m_sdk_operation_completed is set):
        bool m_sdk_operation_completed{};
        EProfileCaptureSdkOperation m_sdk_operation{EProfileCaptureSdkOperation::none};
        std::error_code m_sdk_operation_error{};
        ProfileCaptureSdkStatus m_sdk_status{};

        // ordinary shutdown state:
        bool m_close_requested{};
        std::error_code m_shutdown_error{};
    };

    // ------------------------------------------------------------------
    // capture driver
    // ------------------------------------------------------------------

    /// Game-owned bounded capture state machine. It owns readiness/final-state
    /// artifacts, the session-token stop rendezvous and the vendor operation
    /// sequence; the host owns the production pause/quit actions and executes
    /// vendor operations on the device-owner lane.
    class ProfileCapture final {
    public:
        ProfileCapture() = default;

        ProfileCapture(const ProfileCapture &) = delete;
        ProfileCapture &operator=(const ProfileCapture &) = delete;
        ProfileCapture(ProfileCapture &&) = delete;
        ProfileCapture &operator=(ProfileCapture &&) = delete;

        /// Validates the fresh output directory, records the capture identity
        /// and opens the capture log. `sdk` is borrowed for this driver's
        /// lifetime; a Graphics capture requires a working adapter.
        [[nodiscard]] std::error_code begin(
            const std::filesystem::path &output, EProfileCaptureTool tool, EProfileCaptureWorkload workload,
            std::string_view session_token, u64 process_id, IProfileCaptureSdk *sdk, TimePoint now);

        [[nodiscard]] Expected<std::optional<ProfileCaptureAction>> advance(
            const ProfileCaptureObservation &observed, TimePoint now);

        /// Records the shutdown outcome and persists final-state.json when it
        /// was not written already. Returns success only for a completed
        /// capture; an incomplete/unsupported result is a visible error.
        [[nodiscard]] std::error_code finish(std::error_code cleanup_error);

        /// Production pause acknowledgement from the admission hook, before
        /// the first simulation tick of the admitted world.
        void notePauseApplied(u64 tick) noexcept;

        [[nodiscard]] EProfileCaptureStatus status() const noexcept {
            return m_status;
        }

        [[nodiscard]] EProfileCaptureTool tool() const noexcept {
            return m_tool;
        }

        [[nodiscard]] EProfileCaptureWorkload workload() const noexcept {
            return m_workload;
        }

        [[nodiscard]] bool frozen() const noexcept {
            return m_workload == EProfileCaptureWorkload::frozen;
        }

        /// True while the host still owes the production pause. The admission
        /// hook consults this before the first simulation tick.
        [[nodiscard]] bool pauseBeforeFirstTickPending() const noexcept {
            return frozen() and not m_frozen_pause_satisfied and m_status == EProfileCaptureStatus::pending;
        }

        [[nodiscard]] bool pauseBeforeFirstTickSatisfied() const noexcept {
            return m_frozen_pause_satisfied;
        }

        /// True while a frozen workload must remain paused, including the
        /// collection interval after readiness.
        [[nodiscard]] bool freezeActive() const noexcept {
            return frozen() and
                   (m_status == EProfileCaptureStatus::pending or m_status == EProfileCaptureStatus::ready);
        }

        [[nodiscard]] bool tickPrecededPause() const noexcept {
            return m_tick_preceded_pause;
        }

        /// True when the simulation advanced after the frozen pause was
        /// applied (the workload was not actually held still).
        [[nodiscard]] bool frozenSimulationAdvanced() const noexcept {
            return m_frozen_advanced;
        }

        [[nodiscard]] u64 acceptedFrames() const noexcept {
            return m_accepted_frames;
        }

        [[nodiscard]] double activePresentationSeconds() const noexcept {
            return m_active_seconds;
        }

        [[nodiscard]] u64 sdkOperations() const noexcept {
            return m_sdk_operations;
        }

        [[nodiscard]] bool readyWritten() const noexcept {
            return m_ready_written;
        }

        [[nodiscard]] const std::filesystem::path &output() const noexcept {
            return m_output;
        }

        [[nodiscard]] const std::string &reason() const noexcept {
            return m_reason;
        }

    private:
        // ------------------------------------------------------------------
        // phases
        // ------------------------------------------------------------------

        enum class EPhase : u8 {
            idle,
            warming,
            ready,
            tracing,
            draining,
            finished,
        };

        [[nodiscard]] Expected<std::optional<ProfileCaptureAction>> run_(
            const ProfileCaptureObservation &observed, TimePoint now);
        [[nodiscard]] std::optional<ProfileCaptureAction> publish_(ProfileCaptureAction action);
        [[nodiscard]] std::optional<ProfileCaptureAction> emitSdk_(
            EProfileCaptureSdkOperation operation, TimePoint now);
        [[nodiscard]] std::optional<ProfileCaptureAction> advanceWarming_(
            const ProfileCaptureObservation &observed, TimePoint now);
        [[nodiscard]] std::optional<ProfileCaptureAction> advanceReady_(TimePoint now);
        [[nodiscard]] std::optional<ProfileCaptureAction> advanceTracing_(TimePoint now);
        [[nodiscard]] std::optional<ProfileCaptureAction> advanceDraining_(TimePoint now);
        void pollStopRequest_(TimePoint now);
        void captureIdentity_(const ProfileCaptureObservation &observed);
        void noteAccepted_(const ProfileCaptureObservation &observed);
        void noteSdkResult_(const ProfileCaptureObservation &observed);
        void complete_(EProfileCaptureStatus status, std::string reason, TimePoint now, bool suppress_quit = false);
        void writeLog_(std::string_view line);

        // ------------------------------------------------------------------
        // artifacts
        // ------------------------------------------------------------------

        [[nodiscard]] std::error_code writeReady_(TimePoint now);
        [[nodiscard]] std::error_code writeFinalState_();
        [[nodiscard]] std::string readyJson_() const;
        [[nodiscard]] std::string finalStateJson_() const;
        [[nodiscard]] static std::string jsonString_(std::string_view value);
        [[nodiscard]] static std::string jsonVector_(const float3 &value);
        [[nodiscard]] static std::string jsonExtent_(const int2 &value);
        [[nodiscard]] std::string jsonSettings_() const;

        // ------------------------------------------------------------------
        // state
        // ------------------------------------------------------------------

        std::filesystem::path m_output{};
        std::filesystem::path m_log_path{};
        std::filesystem::path m_ready_path{};
        std::filesystem::path m_final_state_path{};
        std::filesystem::path m_stop_request_path{};

        EProfileCaptureTool m_tool{EProfileCaptureTool::systems};
        EProfileCaptureWorkload m_workload{EProfileCaptureWorkload::frozen};
        EProfileCaptureStatus m_status{EProfileCaptureStatus::pending};
        EPhase m_phase{EPhase::idle};
        IProfileCaptureSdk *m_sdk{};

        TimePoint m_begin{}, m_last_now{}, m_ready_time{}, m_trace_start{};
        TimePoint m_poll_time{};
        std::string m_session_token{};
        std::string m_reason{};
        u64 m_process_id{};

        Array<ProfileCaptureSetting> m_settings{};

        // accepted presentation accounting:
        u64 m_accepted_frames{};
        u64 m_ui_only_frames{};
        u64 m_rejected_frames{};
        double m_active_seconds{};

        // observed identity at readiness and at stop:
        bool m_world_admitted{};
        u64 m_tick{};
        u64 m_ready_tick{};
        u64 m_stop_tick{};
        u64 m_scene_frame_index{};
        u64 m_screen_frame_index{};
        u64 m_ready_scene_frame{};
        u64 m_ready_screen_frame{};
        u64 m_world_id{};
        u64 m_publication_id{};
        float3 m_camera_origin{zero_v};
        float3 m_camera_forward{zero_v};
        int2 m_logical_extent{zero_v};
        int2 m_framebuffer_extent{zero_v};

        // frozen admission gate:
        bool m_frozen_pause_satisfied{};
        bool m_tick_preceded_pause{};
        bool m_frozen_advanced{};
        u64 m_pause_tick{};

        // vendor state:
        bool m_sdk_initialized{};
        bool m_sdk_active{};
        bool m_sdk_started{};
        bool m_sdk_poll_pending{};
        std::error_code m_sdk_error{};
        ProfileCaptureSdkStatus m_sdk_status{};
        u64 m_sdk_operations{};

        // stop rendezvous / artifact state:
        bool m_ready_written{};
        bool m_final_written{};
        bool m_quit_emitted{};
        bool m_suppress_quit{};
        bool m_stop_requested{};
        bool m_finalized{};
        std::error_code m_cleanup_error{};
    };
} // namespace pP::game
