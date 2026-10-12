module;

export module engine.app:frame.render_lane;

import :frame.packet;
import :renderer;
import :renderer.frame_fence;
import :renderer.grid_pass;
import :renderer.triangle_pass;
import :ui.imgui;
import :window.handle;

import engine.core;
import std;

export namespace pP {
    // ------------------------------------------------------------------
    // P2 dedicated render lane: platform/client stays on main, render
    // preparation + encode/submit/present runs on one fixed worker.
    //
    // Transports (one logical consumer each, all bounded):
    // - frame requests forward (main produces, lane consumes, drop_if_full);
    // - frame results back (lane produces, main consumes, reliable:
    //   a full channel overflows to an in-lane stash, never dropped);
    // - control/wake independent of frame capacity (either side signals).
    //
    // Outcomes stay distinguishable: transport-full maps to
    // device_or_resource_busy (transient backpressure, drop the frame),
    // closed transport to operation_canceled, lane-recorded failures keep
    // their own codes. The signalling path NEVER uses wait_if_full/flush/
    // close: a full frame queue cannot block exit, and a failed worker
    // cannot strand the producer in channel cleanup.
    //
    // Thread rules: the lane executes ONLY the injected executor/sweep/init
    // callbacks. It never pumps native events, touches windows, ticks the
    // clock, advances input, or resolves services mid-frame (services are
    // captured at the startup boundary by whoever installs the callbacks).
    // Its only cross-thread signal is the wake callback, which must stay a
    // pure hint (PulseEvent + postEmptyEvent) for the same reason.
    //
    // Ownership != affinity: content owners (passes, translator, UI
    // backend) keep ownership of their objects; the lane is only the fixed
    // execution affinity. Running an editor-owned pass on this lane does
    // NOT transfer scene ownership into Renderer.
    // ------------------------------------------------------------------

    // Owned immutable render inputs for one frame: the packet (values only,
    // shared ownership so the channel move stays noexcept) plus the
    // detached UI draw data published by the client. Immutable after
    // publication; the lane never touches live cameras, windows, or the
    // live ImGui context/IO. The staged pass frames carry everything the
    // lane adopts for the frame — the lane never reads live main-side
    // pass/translator state below.
    enum class EGridWorldCommand : u8 {
        begin,
        append,
        advance,
        activate,
        cancel
    };

    struct GridWorldCommand {
        EGridWorldCommand m_kind{EGridWorldCommand::begin};
        u64 m_sequence{};
        u64 m_job_id{};
        u64 m_world_generation{};
        u64 m_content_fingerprint{};
        GridPass::GridStagedFrame m_stage{};
        bool m_final_batch{false};
    };

    struct GridWorldResult {
        EGridWorldCommand m_kind{EGridWorldCommand::begin};
        u64 m_sequence{};
        u64 m_job_id{};
        u64 m_world_generation{};
        u64 m_content_fingerprint{};
        std::error_code m_error{};
        bool m_applied{false};
        bool m_ready{false};
    };

    using GridWorldExecutor = std23::move_only_function<void(const GridWorldCommand &, GridWorldResult &)>;

    struct RenderFrameRequest {
        bool m_observe_grid{false};
        std::shared_ptr<const FramePacket> m_packet{};
        DetachedUiDrawData m_ui{};
        GridPass::GridStagedFrame m_grid_stage{};
        TrianglePass::TriangleStagedFrame m_triangle_stage{};
        // Identifies the construction reservation this frame settles;
        // parity with RenderFrameResult so the owner pairs a result with
        // exactly one record (see Application::onLaneFrameConsumed_).
        u64 m_reservation_epoch{0u};
        u64 m_reservation_frame_id{0u};
    };

    struct RenderSurfaceRequest {
        u64 m_transaction_id{0u};
        SurfaceIdentity m_identity{};
        SurfaceSettings m_settings{};
    };

    struct RenderSurfaceResult {
        u64 m_transaction_id{0u};
        SurfaceIdentity m_identity{}; // Echoes the request, NOT the new generation.
        AppliedSurfaceSettings m_applied{};
        std::error_code m_error{};
    };

    // Executes on the lane. Fill actual state even on error (including failed
    // restoration); generation validation belongs to Renderer. Bind before
    // the first request. Captured owners must outlive lane join.
    using RenderSurfaceExecutor = std23::move_only_function<void(const RenderSurfaceRequest &, RenderSurfaceResult &)>;

    // Opt-in CPU wall durations include blocking/descheduling. Identity and
    // endpoints travel with the result, rather than a last-value lane gauge.
    struct RenderCpuObservations {
        u64 m_frame_id{};
        TimePoint m_start{}, m_end{};
        TimeSpan m_staging{}, m_uploads{}, m_grid_prepare{}, m_grid_draw{}, m_triangle_draw{}, m_ui_draw{}, m_surface{};
    };

    // Per-frame completion posted back to main: submission acceptance is
    // independent of present (a submitted value travels with the fence
    // until completion; a dropped frame carries m_dropped and no value).
    struct RenderFrameResult {
        std::optional<RenderCpuObservations> m_cpu_observations{};
        std::optional<GpuFrameTiming> m_gpu_timing{};
        std::optional<GridPassObservations> m_grid_observations{};
        // Distinct control completion on the existing reliable result stream.
        // When set, this record has NO frame/reservation/staging settlement.
        // Main must dispatch it before frame accounting, even while admission
        // is suspended; validate transaction ID + echoed handle/generation
        // against the pending candidate before committing m_applied.
        std::optional<RenderSurfaceResult> m_surface_result{};
        // Independent world preparation has no frame reservation or dirty ack.
        std::optional<GridWorldResult> m_world_result{};
        u64 m_frame_id{0u};
        FenceCompletionValue m_submit_value{kFenceValueNone};
        std::error_code m_error{};
        bool m_dropped{false};
        // True only when this frame was submitted and its surface present
        // succeeded. CPU staging and queue acceptance alone do not set it.
        bool m_presented{false};
        // Echoed from the request's reservation identity so the owner can
        // match/reserve exactly once; stale results never release another
        // run's credit (owner validates against its live epoch).
        u64 m_reservation_epoch{0u};
        u64 m_reservation_frame_id{0u};
        // Set when the lane's submit fate is FrameSettlement::kSubmitFailed
        // (possibly-executed submit): the frame is ambiguous, its credit is
        // quarantined, admission stops — never a silent normal release.
        bool m_submit_possibly_executed{false};
        // Font texture generation applied by this frame's encode (explicit
        // ownership acknowledgement for the client-published atlas).
        u64 m_ui_font_version_applied{0u};
        // Surface generation observed on the lane for this frame (feeds
        // the main-side capture mirror; the lane re-verifies every claim).
        WindowHandle m_surface_handle{};
        SurfaceGeneration m_surface_generation{kSurfaceGenerationNone};
        // Outstanding-settlement sweep outcome for this frame. Frame
        // success (no error) and sweep failure are distinguishable, so a
        // retirement failure never masquerades as a clean frame and a
        // failed frame never masks a retirement failure.
        std::error_code m_sweep_error{};
        // Lane-published GridPass staging outcome for the settled packet.
        // GPU retirement and CPU-consumption ack are distinct milestones:
        // this is the CPU-side milestone. The response channel treats every
        // rejected/cancelled/failed slot the same way — m_grid_stage_applied
        // stays false, and the per-slot ack manifest below round-trips
        // verbatim so the owner can classify the slot without consulting
        // the side state the William lane owns.
        bool m_grid_stage_applied{false};
        // CPU staging succeeded, even when bounded atlas publication is pending.
        // A reset already performed must not be replayed during that deferral.
        bool m_grid_staging_applied{false};
        u32 m_grid_staged_tiles{0u};
        u32 m_grid_cached_tiles{0u};
        // Per-slot consumption ack manifest, echoed from the request's
        // staged payload. The Game/colony side owns collection of the set;
        // the lane only round-trips it so every settlement outcome
        // (applied, obsolescence, minimized, cancellation, absorb-and-drop,
        // worker error) carries the same metadata.
        bool m_grid_reset_caches{false};
        bool m_grid_full_refresh{false};
        u64 m_grid_ack_seed{0u};
        Array<u32> m_grid_ack_chunk_ids{};
        Array<u64> m_grid_ack_revisions{};
    };

    // Independent control/wake path. Hints never wait; transactions explicitly
    // report full. Exit is additionally latched so it survives a full queue.
    struct RenderControl {
        enum class Kind : u8 {
            kWake,
            kExit,
            kConfigChanged,
            kReconfigureSurface,
            kGridWorld,
        };

        Kind m_kind{Kind::kWake};
        RenderSurfaceRequest m_surface_request{};
        std::shared_ptr<const GridWorldCommand> m_world_command{};
    };

    // Lane work: consumes one owned request, fills the result (submit
    // value, error, font ack). Runs exclusively on the lane. Must not
    // migrate Renderer ownership, resolve services, or touch platform.
    using RenderFrameExecutor =
        std23::move_only_function<std::error_code(const RenderFrameRequest &, RenderFrameResult &)>;

    // Completion sweep: retires fence work and reports the outstanding
    // count feeding the main-side admission hint. Runs on the lane.
    using RenderLaneSweep = std23::move_only_function<std::error_code(u32 &out_outstanding)>;

    // Startup/shutdown operation executed ON the lane (Renderer init,
    // pass-GPU release, Renderer shutdown). Runs on the lane.
    using RenderLaneOperation = std23::move_only_function<std::error_code()>;

    // Pure wake hint toward main (credit returned, terminal latched, exit
    // acknowledged). Runs on the lane; must only signal, never block.
    using RenderLaneWake = std23::move_only_function<void()>;

    // Main-side pump slice: native event collection + a bounded wait.
    // Runs on MAIN during sync handoffs / lane quiescence polling so the
    // platform stays serviced while a lane-owned resource retires.
    using RenderLanePumpSlice = std23::move_only_function<void()>;

    class RenderLane final {
    public:
        // Transport depths: 2 in-flight frames bound the backlog (pairwise
        // overlap with the 2-credit ledger), 4 results absorb completion
        // bursts, 8 controls absorb wake storms without ever blocking.
        static constexpr std::size_t kFrameDepth{2u};
        static constexpr std::size_t kResultDepth{4u};
        static constexpr std::size_t kControlDepth{8u};

        RenderLane();

        RenderLane(const RenderLane &) = delete;

        RenderLane &operator=(const RenderLane &) = delete;

        // Destructor stops, joins, then destroys any record still aboard
        // the transports (RawChannel runs no element dtors): every teardown
        // path — normal stop, join, or destruction with live work — leaves
        // no live record behind. The drain runs only after the worker is
        // proven gone, so it never races a live emitter, and it never
        // blocks on queue capacity (close() is never used on the
        // signalling path). A bounded-join timeout fail-stops (log +
        // terminate, consistent with the Application UNKNOWN policy)
        // instead of falling through into member teardown behind live
        // work.
        ~RenderLane() noexcept;

        [[nodiscard]] bool isRunning() const noexcept {
            return m_running.load(std::memory_order_acquire);
        }

        // Lane instance is single-use: start() succeeds once per instance
        // (already_connected while running, operation_not_permitted on
        // same-instance restart). A fresh instance is the restart path;
        // transports are drained/recreated, not recycled.
        // Returns the init error, if any (the worker is parked then).
        // Starts the worker and runs `init` on it synchronously (Renderer
        // init happens ON the lane, so its owner id is never migrated).
        [[nodiscard]] std::error_code start(
            RenderLaneOperation init, RenderFrameExecutor executor, RenderLaneSweep sweep, RenderLaneWake wake);

        // Late executor bind for owners constructed after start (derived
        // passes exist only after the base initialize). Must precede the
        // first submit; safe under the lane mutex.
        void setExecutor(RenderFrameExecutor executor);

        void setSurfaceExecutor(RenderSurfaceExecutor executor);
        void setWorldExecutor(GridWorldExecutor executor);
        [[nodiscard]] std::error_code requestGridWorld(std::shared_ptr<const GridWorldCommand> command) noexcept;

        // One outstanding transaction, including its undrained ack. No frame
        // credit is charged. Full/busy -> bounded caller retry; stopped ->
        // operation_canceled. Success means queued, NEVER applied. Payload is
        // owned; no panel/scratch borrow. Ack uses drainResults + wakeMain.
        [[nodiscard]] std::error_code requestSurfaceReconfigure(RenderSurfaceRequest request) noexcept;

        // Nonblocking submit. Full -> device_or_resource_busy (drop the
        // frame, never fail the app); closed -> operation_canceled.
        [[nodiscard]] std::error_code submitFrame(RenderFrameRequest request) noexcept;

        // Nonblocking hint post (wake/exit/config only; surface requests must
        // use requestSurfaceReconfigure). Never blocks, never fails on capacity:
        // kExit additionally latches the exit atomic, so signalling exit
        // behind a full queue still terminates promptly.
        void postControl(RenderControl::Kind kind) noexcept;

        // Synchronous operation ON the lane (pass-GPU release, Renderer
        // shutdown, quiescence probes). Teardown paths only: blocks the
        // caller until the lane runs it. Fails closed when stopped.
        // `out_operation_ran` distinguishes acknowledged completion (true:
        // the lane ran the operation, which may itself have returned an
        // error) from handoff failure (false: not_connected /
        // device_or_resource_busy / timed_out — the operation never
        // acknowledged; on timeout it may STILL run later). Callers must
        // only read operation-published outcome storage when this is true.
        [[nodiscard]] std::error_code runOnLaneSync(RenderLaneOperation operation, bool *out_operation_ran = nullptr);

        // Install the main-side pump slice used while the caller waits for
        // a sync handoff or a parked worker to settle (oracle P2 teardown
        // protocol: keep required platform servicing alive while the render
        // lane quiesces; never block the pump in join-class waits).
        void setWaitPump(RenderLanePumpSlice pump);

        // Outcome of the lane's final post-exit sweep: the sweep that
        // retires submitted work after the frame loop stops. Completes the
        // lane report chain: per-frame m_error / m_sweep_error plus this
        // one final value summarises the retirement flood the steady
        // per-frame path may have missed.
        [[nodiscard]] std::error_code finalSweepError() const noexcept;

        // Teardown settle step (runs a sync handoff internally): nulls the
        // frame executor and drops every queued-but-unexecuted frame,
        // posting dropped results best-effort. Also disables surface-control
        // admission and settles queued controls as canceled acknowledgements
        // before returning; call before releasing callback-captured resources.
        // After this, no lane frame
        // can start, so surface/pass release may proceed. Idempotent.
        // Returns the handoff error (never swallows): `out_operation_ran`
        // distinguishes acknowledged completion from handoff failure; the
        // teardown caller retains/escalates via first-error discipline.
        [[nodiscard]] std::error_code drainQueuedFrames(bool *out_operation_ran = nullptr) noexcept;

        // Drains posted results into `out` (best effort, nonblocking).
        // Returns the first drained error, if any; empty drain is success.
        [[nodiscard]] std::error_code drainResults(Array<RenderFrameResult> &out) noexcept;

        // Main-side admission hint (ledger stays the truth on the lane):
        // outstanding submitted-but-uncompleted frames as last swept. Hint
        // only — never drives lane park or retirement decisions.
        [[nodiscard]] u32 inflightEstimate() const noexcept {
            return m_inflight_estimate.load(std::memory_order_acquire);
        }

        // Latched terminal failure (quarantine/device-loss class), if any.
        // Hints only: the caller rechecks persistent state, never trusts
        // this alone for lifetime decisions.
        [[nodiscard]] std::error_code terminalError() const noexcept;

        [[nodiscard]] u64 submittedCount() const noexcept {
            return m_submitted_count.load(std::memory_order_acquire);
        }

        [[nodiscard]] u64 droppedCount() const noexcept {
            return m_dropped_count.load(std::memory_order_acquire);
        }

        // Last completed lane execution wall time in microseconds
        // (monotonically-overwritten last-value gauge for display only).
        // Written by the worker at the end of each executed frame beside
        // the lane_execute_end stamp; read by any thread for the honest
        // main/render split readouts. Relaxed ordering suffices (a stale
        // read only ages a debug number by one frame). Never consulted
        // for admission, retirement, park, or teardown decisions — the
        // ledger/estimate path stays the only hint there. Allocation-free,
        // noexcept.
        [[nodiscard]] u64 lastExecuteUs() const noexcept {
            return m_last_execute_us.load(std::memory_order_relaxed);
        }

        // Teardown protocol step: stop admission + signal exit + wake, then
        // join. Never blocks on queue capacity. The lane settles admitted
        // packets on its way out (discards unsubmitted, posts dropped
        // results best-effort); submitted work retires through the finish
        // operation below or the later Renderer shutdown.
        void requestStop() noexcept;

        // Pump-compatible join: bounded wait slices invoke the installed
        // wait pump so the platform stays serviced while the worker
        // quiesces. Escalation is bounded: on the deadline the first
        // error (and the terminal class) is latched, pumping stops, and
        // no live resource is freed on timeout — the caller keeps
        // ownership of the lane and must not release behind it.
        std::error_code join() noexcept;

    private:
        // Invokes the installed pump slice after moving it out from
        // under the pump mutex and restoring it before returning, so a
        // later wait slice still finds the slice installed (no lock is
        // held across the pump call itself).
        void invokePumpSlice_() noexcept;

        void runLane_() noexcept;

        void handleFrameEvent_() noexcept;

        // Returns true when any control message was serviced.
        bool handleControlEvent_() noexcept;

        void runSyncHandoffIfPending_() noexcept;

        // One logical retirement sweep for the current moment: retires
        // fence work ON the lane and refreshes the main-side admission
        // estimate as (queued + executing + outstanding-swept). Runs from
        // the frame, control, and idle wake paths so retirement never
        // depends on new frame arrival.
        void sweepEstimateOnLane_() noexcept;

        // Recompute the admission estimate from the live reservation
        // window (queued + executing + outstanding-swept). Lane thread only:
        // single writer; the producer charges the queued counter but never
        // publishes the aggregate.
        void refreshEstimate_() noexcept;

        void noteFirstError_(std::error_code error) noexcept;

        void setTerminal_(std::error_code error) noexcept;

        // Durable settlement path: results never shed on capacity. When the
        // bounded result channel is full they overflow into this stash
        // (preserving order) and drainResults surfaces them — the owner's
        // per-slot acks, reset re-arms, and diagnostics stay exactly-once.
        std::mutex m_result_stash_mutex{};
        Array<RenderFrameResult> m_result_stash{};
        std::atomic<u64> m_results_stashed{0u};

        void wakeMain_() noexcept;

        // Reliable result post: channel first, stash on capacity, flush the
        // stash before every post. Never drops a settlement. Takes the
        // record by rvalue reference: results carry owned arrays, so every
        // caller moves (never copies) into the post.
        void postResult_(RenderFrameResult &&result) noexcept;

        // Consumer-side destroy pass over all three transports: pops every
        // remaining record and runs its dtor (queued requests, undrained
        // results, control hints). Nonblocking and allocation-free; runs
        // only with no live emitter (worker joined), so it never races
        // production or consumption.
        void drainTransportsDestroy_() noexcept;

        // Channels precede the thread so reverse-destruction joins the
        // worker BEFORE its transports die (emitters joined first).
        SharedRawChannelPtr m_frames{};
        SharedRawChannelPtr m_results{};
        SharedRawChannelPtr m_controls{};

        std::atomic<bool> m_running{false};
        // A lane instance is single-use: queued frames and results survive
        // as transport bytes, so a restart on the same instance would replay
        // or skip them invisibly. Epoch filtering cannot fix this — the
        // transports are recycled rather than recreated. A fresh lane must
        // be constructed (Application does); restart fails closed.
        std::atomic<bool> m_started_once{false};
        // Set once the worker has left runLane_(); lets join() poll for
        // completion across bounded slices without timed-join support.
        std::atomic<bool> m_worker_finished{false};
        std::atomic<bool> m_exit_requested{false};

        std::mutex m_callback_mutex{};
        RenderFrameExecutor m_executor{};
        RenderSurfaceExecutor m_surface_executor{};
        GridWorldExecutor m_world_executor{};
        bool m_world_command_pending{false};
        // Protected by the existing result-stash mutex. Released only when
        // main consumes the control ack; therefore ack storage stays bounded.
        bool m_surface_transaction_pending{false};
        RenderLaneSweep m_sweep{};
        RenderLaneWake m_wake{};

        // Synchronous handoff for teardown-time lane operations.
        std::mutex m_sync_mutex{};
        std::condition_variable m_sync_condition{};
        RenderLaneOperation m_sync_operation{};
        std::error_code m_sync_result{};
        bool m_sync_pending{false};
        bool m_sync_done{false};
        // Set by requestStop: no new sync handoff is accepted afterwards,
        // so a release can never be posted behind the exit it precedes.
        std::atomic<bool> m_stop_initiated{false};
        std::atomic<bool> m_surface_admission_closed{false};

        std::atomic<u32> m_inflight_estimate{0u};
        std::atomic<u32> m_last_sweep_outstanding{0u};
        // Reservation-before-construction: submitted-but-not-started and
        // executing-but-not-retired windows folded into the admission
        // estimate so main never admits a frame under a stale stale slot.
        std::atomic<u64> m_queued_frames{0u};
        std::atomic<u64> m_executing_frames{0u};
        std::atomic<u64> m_submitted_count{0u};
        std::atomic<u64> m_dropped_count{0u};
        // Display-only last-execute gauge (see lastExecuteUs): single
        // worker writer, any-thread relaxed readers. Zero until the first
        // executed frame completes.
        std::atomic<u64> m_last_execute_us{0u};
        // First lane error + terminal class as generic errc values (0 =
        // none); reconstructed with the generic category on read.
        std::atomic<int> m_first_error{0};
        std::atomic<int> m_terminal_error{0};
        std::atomic<int> m_final_sweep_error{0};

        // Main-side pump slice (see setWaitPump).
        std::mutex m_pump_mutex{};
        RenderLanePumpSlice m_pump_slice{};

        // Declared LAST so reverse destruction destroys the join FIRST: the
        // worker is joined while every mutex/cv/callback/atomic it may
        // touch is still alive (previously the join happened after those
        // members died — a use-after-destroy on the worst path).
        std::jthread m_thread{};
    };
}
