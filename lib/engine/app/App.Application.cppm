module;

export module engine.app:application;

import :renderer.frame_fence;

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
        [[nodiscard]] std::error_code probeFrameAdmissionPreUpdate_(bool &out_skip);

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

    private:
        std::unique_ptr<Renderer> m_renderer{};
        TimerManager m_application_clock{};
        FramePacer m_pacer{};
        u64 m_frame_index{0u};
        // Accumulated dt of capacity-busy skipped frames (B2a carry).
        TimeSpan m_pending_skipped_dt{};
        std::optional<TimeSpan> m_target_frame_time{};

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
