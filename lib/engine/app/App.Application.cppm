module;

export module engine.app:application;

import :frame.render_lane;
import :renderer.frame_fence;
import :window.handle;

import engine.core;
import std;

export namespace pP {
    class IClientService;
    class IInputService;
    class IPlatform;
    class IUIService;
    class IWindowService;
    class Renderer;

    struct ApplicationDomain {
        /// headless applications run without a window
        bool m_is_headless: 1 {false};
        /// interactive applications may require user-input
        bool m_is_interactive: 1 {true};
        /// handle player profile and online services
        bool m_needs_presence: 1 {false};
        /// application needs to render something with the GPU
        bool m_needs_rendering: 1 {true};
        /// spawn ui service and render user-interfaces
        bool m_needs_user_interface: 1 {true};
    };

    class Application : public safe_object {
        // One per admitted frame: epoch + frame identity, active flag, and
        // state machine (kLocal→kQueued→kSubmitted→ether release/quarantine).
        // Charged slot only returns once, via the settlement helper.
        struct FrameReservation;

    public:
        Application(ApplicationDomain domain, std::string_view name, std::span<const char *const> argv);

        // Fail-stop injection: a test double records entry and returns (the
        // wrapper still terminates); production passes nothing and the
        // wrapper terminates directly. Never null-dereferenced.
        Application(
            ApplicationDomain domain, std::string_view name, std::span<const char *const> argv,
            GpuTeardownFailureHandler on_gpu_teardown_failure);

        // ReSharper disable once CppHidingFunction - safe_object dtor is non-virtual; hiding is benign
        virtual ~Application() noexcept;

        Application(const Application &) = delete;

        Application &operator=(const Application &) = delete;

        Application(Application &&) = delete;

        Application &operator=(Application &&) = delete;

        [[nodiscard]] const std::string &getName() const noexcept {
            return m_name;
        }
        [[nodiscard]] std::span<const std::string> getArguments() const noexcept {
            return m_arguments;
        }

        [[nodiscard]] const ApplicationDomain &getDomain() const noexcept {
            return m_domain;
        }
        [[nodiscard]] const IPlatform &getPlatform() const noexcept {
            return *m_platform;
        }
        [[nodiscard]] const SharedContext &getLifecycle() const noexcept {
            return m_lifecycle;
        }
        [[nodiscard]] const Renderer &getRenderer() const noexcept {
            return *m_renderer;
        }
        [[nodiscard]] const ServicesStore &getServices() const noexcept {
            return m_services;
        }
        [[nodiscard]] const TimerManager &getTimerManager() const noexcept {
            return m_application_clock;
        }

        [[nodiscard]] std::optional<TimeSpan> getTargetFrameTime() const noexcept;

        [[nodiscard]] Renderer &getRenderer() noexcept {
            return *m_renderer;
        }
        [[nodiscard]] ServicesStore &getServices() noexcept {
            return m_services;
        }
        [[nodiscard]] TimerManager &getTimerManager() noexcept {
            return m_application_clock;
        }

        [[nodiscard]] const std::filesystem::directory_entry &getInstallDir() const noexcept {
            return m_install_dir;
        }
        [[nodiscard]] const std::filesystem::directory_entry &getConfigDir() const noexcept {
            return m_config_dir;
        }
        [[nodiscard]] const std::filesystem::directory_entry &getContentDir() const noexcept {
            return m_content_dir;
        }
        [[nodiscard]] const std::filesystem::directory_entry &getWorkingDir() const noexcept {
            return m_working_dir;
        }

        void requestExit(std::error_code clause = {}) const noexcept;

        // P2 admission wake hint: coalescing pulse + native wakeup for a
        // thread parked in the pacer wait or the backpressure hint wait.
        // Hints only — every waiter rechecks persistent exit/error/credit
        // state before sleeping or admitting. Safe from any thread; the
        // render lane signals credit return, terminal state, and exit ack
        // through this (its only legal cross-thread signal alongside
        // IPlatform::postEmptyEvent).
        void requestFrameAdmissionWake() const noexcept;

        // Run-loop termination query: true once a graceful exit was
        // requested or a cancel error latched. Consulted by run() alongside
        // (not instead of) cancel state — exit is not cancellation.
        [[nodiscard]] bool isExitRequested() const noexcept;

        void setBackgroundPriority(bool throttle) noexcept;

        /// Opt out of the background focus throttle (profiling/bounded runs).
        void setNoFocusThrottle(bool enabled) noexcept;

        [[nodiscard]] bool getNoFocusThrottle() const noexcept {
            return m_no_focus_throttle;
        }

        void setTargetFrameDuration(TimeDuration frame_time) noexcept;

        void setTargetFrameDuration(std::nullopt_t) noexcept;

        void setTargetFrameRate(int fps) noexcept;

        [[nodiscard]] std::error_code run();

        // Dedicated render lane posture. Enabled by default for windowed
        // rendering hosts; headless/offscreen hosts (smoke, readback,
        // scripted harnesses) keep inline execution. Derived hosts with
        // their own main-lane render overrides must explicitly opt out
        // (documented at their declaration); Editor-style hosts inherit the
        // lane and split build (client) from execute (lane) via hooks.
        void setRenderLaneEnabled(bool enabled) noexcept {
            m_render_lane_enabled = enabled;
        }

        [[nodiscard]] bool renderLaneEnabled() const noexcept {
            return m_render_lane_enabled;
        }

        // Skipped-frame dt carry (B2a): the run-level busy branch accumulates
        // here and the next admitted frame prepends it to its dt. Single
        // Application-owned carry. Public so saturation tests can prove
        // preservation headless.
        void noteSkippedFrameDt(TimeSpan dt) noexcept;
        [[nodiscard]] TimeSpan claimSkippedFrameDt() noexcept;

        // Recorded B1' disposition (kUnknown until the first prepare
        // classifies the epoch — never idle-proven before proof).
        // Public: the B1'/B3' tests assert the recorded outcome.
        [[nodiscard]] GpuQuiescence gpuTeardownDisposition() const noexcept {
            return m_gpu_teardown_disposition;
        }

        // Ordered teardown-stage log for the B3' lifecycle evidence (each
        // stage appended exactly once on its production path):
        // "quiescence", "idle-proven"|"removal-confirmed"|"unknown",
        // then releases in inverse-of-setup order ("scene", "ui",
        // "renderer", "rhi", "platform"). Releases always follow idle proof.
        [[nodiscard]] std::span<const std::string> gpuTeardownStageLog() const noexcept {
            return m_gpu_teardown_stages;
        }

        // B3' quiescence-entry hook, TESTS ONLY: invoked once inside
        // prepareGpuTeardown_ after admission stops and before the idle
        // wait, while every owner is still alive and nothing is released.
        // Production never sets it.
        void setGpuTeardownEntryHookForTest(std23::move_only_function<void()> &&hook) {
            m_gpu_teardown_entry_hook = std::move(hook);
        }

    protected:
        // Test-seam construction: scripted platform + injected clock (the
        // production ctor delegates with IPlatform::create() and the steady
        // clock). The clock outlives the application (non-owning view).
        Application(
            ApplicationDomain domain, std::string_view name, std::span<const char *const> argv,
            std::unique_ptr<IPlatform> platform, const ITimerClock &clock,
            GpuTeardownFailureHandler on_gpu_teardown_failure = nullptr);

        [[nodiscard]] virtual std::error_code initialize();

        [[nodiscard]] virtual std::error_code update(TimeSpan dt);

        [[nodiscard]] virtual std::error_code render();

        [[nodiscard]] virtual std::error_code shutdown();

        // P0 serial-explicit stages (same thread, one frame credit):
        // advanceClock_ paces via FramePacer then ticks with zero target;
        // update()/render() remain the advance-client / execute stages.
        [[nodiscard]] std::error_code advanceClock_(TimeSpan *out_delta_time);

        // Fused lifecycle completion: retain the first error through
        // shutdown + logger drain BEFORE choosing run()'s return value.
        // Reads no lifecycle state that shutdown() resets.
        [[nodiscard]] std::error_code finishRun_(std::error_code prior);

        // P1 admission pre-probe (B2b): retire + probe the renderer BEFORE
        // the whole virtual update stage. Returns busy as success+skip=true
        // (platform serviced by the caller, no client work), quarantine and
        // terminal as errors, and ok as skip=false. No renderer (headless or
        // pre-init) always admits.
        //
        // P2 lane branch: the GPU ledger lives on the lane, so main never calls
        // retireCompletedFrames/checkFrameAdmission (both would race the
        // lane's sweep). Instead this drains posted results, checks the
        // latched terminal mirror, reconciles reservation records against the
        // published retirement frontier, and acquires a main-side reservation
        // against the credit bound — the reservation held from BEFORE
        // client frame construction through the outcome-dependent settle, so
        // a third frame is never constructed while two are outstanding.
        [[nodiscard]] std::error_code probeFrameAdmissionPreUpdate_(bool &out_skip);

        // P2 combined admission (deadline AND credit, single helper): the
        // pacer deadline half (advanceClock_) plus the credit half above.
        // The renderer ledger is the source of truth; pacer/lane signals
        // are hints rechecked here before sleeping or building.
        [[nodiscard]] std::error_code admitClientFrame_(TimeSpan *out_delta_time, bool &out_skip);

        // P2 backpressure hint wait: after a transport drop, parks in
        // bounded pump+wait slices until a credit hint, exit, terminal, or
        // the slice budget runs out. Never blocks the native pump: every
        // slice services platform events first.
        void waitForAdmissionHint_();

        // P2 completion pump: drains lane results (first error wins,
        // operation_canceled results are neutral shutdown artifacts),
        // refreshes the surface-generation mirror, and counts drops.
        [[nodiscard]] std::error_code pumpRenderCompletions_();

        // P2 pacer seam, installed once: every pacer sleep slice pumps
        // native events then waits natively, so waits stay event-driven.
        void installPacerSleepOnce_();

        void pacerSleepSlice_(TimeSpan slice) noexcept;

        // P2 lane lifecycle: start runs Renderer init ON the lane (owner id
        // never migrates); stop follows the teardown protocol (stop
        // admission -> signal exit -> settle -> join) with the platform
        // kept pumping by the caller while the lane quiesces.
        [[nodiscard]] std::error_code startRenderLane_();

        [[nodiscard]] std::error_code stopRenderLane_() noexcept;

        // True while the lane owns the Renderer (all Renderer/pass-GPU
        // calls must run on it; main touches no renderer state).
        [[nodiscard]] bool renderLaneOwnsRenderer_() const noexcept;

        // B1' fail-stop teardown: idempotent. Stops admission permanently,
        // establishes the GPU quiescence disposition (idle-proven vs
        // removal-confirmed vs UNKNOWN) WITHOUT releasing anything, and
        // records it for the shutdown entries below. Safe on every path:
        // run()'s exception unwind, init rollback, direct shutdown, and
        // repeated calls (the second call re-reports the recorded outcome).
        // A renderer that was never initialized is trivially idle-proven.
        // Fail-closed: instrumentation below is best-effort only and this
        // function never lets an exception escape before the caller
        // fail-stops on UNKNOWN — completion flags publish only after
        // classification.
        [[nodiscard]] std::error_code prepareGpuTeardown_();

        // B1'a epoch reset: clears the prepared flag, disposition (to
        // UNKNOWN = nothing proven), first error, and stage log BEFORE new
        // GPU resources can be acquired. Called at the top of run() (every
        // epoch, even when a derived fixture pins initialize) and at the
        // top of initialize() (every direct-init path incl. partial-init
        // rollback). Idempotence holds within an epoch only.
        void resetGpuTeardownEpoch_() noexcept;

        // Best-effort teardown-stage record: never throws (allocation
        // failure is silently dropped — diagnostics must not gate the
        // UNKNOWN fail-stop or the idle-proven release).
        void noteGpuTeardownStageBestEffort_(const char *stage) noexcept;

    protected:
        // B3' lifecycle evidence: derived stages append exactly once each,
        // in inverse-of-setup order, always after idle proof above.
        void noteTeardownStage_(std::string stage) {
            m_gpu_teardown_stages.push_back(std::move(stage));
        }

        // B1' fail-stop: invoke when the recorded disposition is UNKNOWN at
        // a shutdown entry. Calls the constructor-injected handler, then
        // terminates even if the handler returns. noexcept, no throw, no
        // logger-drain dependency. Watchdog note: a driver call blocked
        // INSIDE the quiescence wait still needs an external supervisor.
        [[noreturn]] void failStopOnUnknownGpuTeardown_(std::error_code quiescence_error) noexcept;

        [[nodiscard]] u64 getFrameIndex() const noexcept {
            return m_frame_index;
        }

        // Current reservation epoch for tagging lane requests/results, so a
        // floating settlement from a previous run can never release this
        // run's credit nor ack its edits.
        [[nodiscard]] u64 reservationEpoch_() const noexcept {
            return m_reservation_epoch;
        }

        // Release a live construction-reservation record exactly once and
        // deactivate it. The empty-ledger release failure is a loud
        // (invalid_argument) programming-invariant breach, never swallowed.
        void releaseReservation_(struct FrameReservation &record);

        // Advance kSubmitted records to released once the lane-published
        // retirement frontier has passed their accepted value. Runs on main
        // at every admission point (lane + inline) and in the hint wait.
        void reconcileSubmittedReservations_();

        // Mirror the renderer-owned retirement frontier into the main-side
        // atomic AFTER a retire of outstanding frames, so the next
        // admission probe reconciles against actually-retired values.
        // Lane-affine when the lane owns the renderer (the sweep callback
        // and the test gate-open handoff both run ON the lane); the inline
        // retire sites keep their direct mirror. Protected (not private)
        // so the lane-affine fixture handoff publishes through this exact
        // mirror instead of growing a second one.
        void publishLaneRetirementFrontier_();

        // Record the lane transport accepting a request: the record moves
        // kLocal → kQueued. A queued-but-never-executed frame still holds
        // its credit until the matching settlement result arrives.
        void noteReservationQueued_(u64 frame_id);

        // Lane coordinator for derived hosts (Editor): null when inline.
        // Derived teardown dispatches its lane-affine releases through it
        // before the base shutdown stops the worker.
        [[nodiscard]] RenderLane *renderLane() noexcept {
            return m_render_lane.get();
        }

        // P2 surface-generation mirror (hint): the last generation the lane
        // reported for a surface handle. Fresh claims re-verify on the lane.
        [[nodiscard]] SurfaceGeneration laneSurfaceGeneration(WindowHandle handle) const noexcept {
            if (handle != m_lane_surface_handle) {
                return kSurfaceGenerationNone;
            }
            return m_lane_surface_generation;
        }

        // Durable per-slot settlement hook: fires exactly once for every
        // lane frame packet whose outcome is known — applied, dropped in
        // the scene, cancelled at teardown, rejected-by-admission, or
        // applied-false on worker error/obsolescence. Derived hosts use it
        // for consumption-deferred world-edit acks; GPU retirement stays
        // a distinct milestone in RenderFrameResult.m_submit_value.
        //
        // The construction reservation is reconciled here exactly once,
        // matched by (epoch, frame id), and released ONLY under these rules:
        //   dropped / cancelled / definite rejection → release now;
        //   possibly-executed submit (ambiguous)      → quarantined, charged,
        //     admission stopped (parked slot, never reused);
        //   accepted submission                        → charged until the
        //     lane-published retirement frontier covers its fence value.
        // Inline-executed frames settle through the same hook. Stale results
        // (another run's epoch, or no active matching record) never release
        // a reservation and never ack edits.
        virtual void onLaneFrameConsumed_(const RenderFrameResult &);

    private:
        std::unique_ptr<Renderer> m_renderer{};
        TimerManager m_application_clock{};
        FramePacer m_pacer{};
        u64 m_frame_index{0u};
        // Accumulated dt of capacity-busy skipped frames (B2a carry).
        TimeSpan m_pending_skipped_dt{};
        std::optional<TimeSpan> m_target_frame_time{};

        // P2 dedicated render lane (Application-owned coordinator state).
        // When running, it owns Renderer affinity: init/shutdown, sweeps,
        // and all submission paths execute on it; main coordinates via the
        // transports only. Null (or stopped) means inline execution with
        // identical stages. Declared before m_platform so teardown order
        // stays valid; explicitly stopped in shutdown() before services die.
        std::unique_ptr<RenderLane> m_render_lane{};
        bool m_render_lane_enabled{true};
        bool m_pacer_sleep_installed{false};
        // Coalescing admission hint (credit return / terminal / exit ack).
        // Main re-arms before re-reading persistent state; hints never
        // substitute for the ledger. Mutable so the const wake path (and
        // the const requestExit above it) can emit from any thread — same
        // shape as the mutable FenceWaitHook: the pulse is a single-writer
        // coalescing atomic, safe for concurrent emit/poll/reset.
        mutable PulseEvent m_admission_wake{};
        // Scratch drain buffer for lane results (reused, never grown
        // unboundedly: the result channel itself is bounded).
        Array<RenderFrameResult> m_render_result_scratch{};
        // First durable error captured by the wait pump (drain/native),
        // retained for the shutdown result instead of silently ignored.
        std::error_code m_wait_pump_error{};
        // Surface-generation mirror fed by lane results (hint only; the
        // lane re-verifies every claim against the live record at submit).
        WindowHandle m_lane_surface_handle{};
        SurfaceGeneration m_lane_surface_generation{kSurfaceGenerationNone};
        // Main-side mirror of the Renderer retirement frontier, published
        // by the lane's sweep callback (lane writes release, main reads
        // acquire). A submitted reservation releases only after the CPU
        // settlement result AND this frontier reaching its fence value.
        std::atomic<FenceCompletionValue> m_lane_retirement_frontier{kFenceValueNone};

        // Main-side frame reservation ledger: capacity-bound construction
        // economy. One credit is acquired in probeFrameAdmissionPreUpdate_
        // BEFORE the client constructs the frame and stays charged until the
        // settlement rules below prove the frame safe (settled + retired,
        // dropped, or definitely rejected). The lane-owned FrameCreditLedger
        // inside Renderer stays the GPU-side admission truth; this main-side
        // bound guarantees a third frame is never constructed while two are
        // outstanding across building/queued/recording/GPU-pending.
        FrameCreditLedger m_frame_reservations{};
        // Keyed reservation records (epoch, frame id) replacing the former
        // lane/inline pending counters: the record itself identifies the
        // frame every settlement decision fires for, so pairing no longer
        // depends on per-lane counter snapshots. A record is consumed
        // exactly once by the settlement rules in onLaneFrameConsumed_()
        // (release, quarantine-hold, or retire-to-frontier); the ledger is
        // only reset after old emitters are gone (see shutdown()).
        struct FrameReservation {
            enum class State : u8 {
                kLocal,
                kQueued,
                kSubmitted,
                kQuarantined
            };

            bool m_active{false};
            u64 m_epoch{0u};
            u64 m_frame_id{0u};
            State m_state{State::kLocal};
            FenceCompletionValue m_submit_value{kFenceValueNone};
            // Becomes true once the lane published its result; the record
            // survives until the matching GPU retirement (submitted) or a
            // quarantine settle.
            bool m_cpu_settled{false};
        };
        Array<FrameReservation> m_reservations{};
        // Monotone run epoch: bumped once per epoch so stale results from a
        // finished lane/run can never match (and never release) a record
        // from the current run.
        u64 m_reservation_epoch{0u};

        // m_platform precedes m_services so reverse-destruction releases the
        // service-store observers before their platform owners (safe_ptr rule).
        const std::unique_ptr<IPlatform> m_platform{};
        ServicesStore m_services{};
        SharedContext m_lifecycle{};
        context::CancelClauseFunc m_request_exit{};

        // Graceful-exit latch, distinct from the cancel error above: an empty
        // requestExit raises this (run() still returns success); a nonzero
        // clause latches the cause on the lifecycle instead. Mutable so the
        // const requestExit can raise it; re-armed by initialize().
        mutable std::atomic<bool> m_exit_requested{false};

        // B1' fail-stop state: constructor-injected handler (nullptr =
        // terminate directly), recorded disposition + first error, ordered
        // stage log, entry hook, and the done flag making prepare idempotent.
        // A fresh epoch starts UNKNOWN (nothing proven); resetGpuEpoch_()
        // clears the flag/log/error before new GPU resources can be
        // acquired, keeping idempotence within one epoch only.
        GpuTeardownFailureHandler m_on_gpu_teardown_failure{nullptr};
        GpuQuiescence m_gpu_teardown_disposition{GpuQuiescence::kUnknown};
        std::error_code m_gpu_teardown_error{};
        Array<std::string> m_gpu_teardown_stages{};
        std23::move_only_function<void()> m_gpu_teardown_entry_hook{};
        bool m_gpu_teardown_prepared{false};

        /// throttle refresh loop to low frame-rate when application is in background
        bool m_has_background_priority{false};

        /// opt out of the background focus throttle
        bool m_no_focus_throttle{false};

        const Array<std::string> m_arguments{};
        const std::string m_name{};
        const ApplicationDomain m_domain{};

        std::filesystem::directory_entry m_install_dir{};
        std::filesystem::directory_entry m_config_dir{};
        std::filesystem::directory_entry m_content_dir{};
        std::filesystem::directory_entry m_working_dir{};
    };
}
