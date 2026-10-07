module;
export module engine.app:renderer;

import :renderer.frame_fence;
import :renderer.types;

import engine.core;
import engine.math;
import engine.rhi;
import std;

export namespace pP {
    class Window;
    using WindowHandle = Numeric<void *, Window>;
    // Mirrors :window.handle (which this partition deliberately does not
    // import: it redeclares the handle alias above). Same underlying type.
    using NativeWindowHandle = Numeric<void *, WindowHandle>;

    // ------------------------------------------------------------------
    // generic renderer: surfaces + queue + submission only
    // ------------------------------------------------------------------

    // Generational surface identity for a frame's worth of work. A packet
    // names the surface generation it was built for; the Renderer rejects
    // obsolete generations BEFORE acquisition. After acquisition the
    // backend-required release/present path runs unconditionally.
    struct SurfaceIdentity {
        WindowHandle m_handle{};
        SurfaceGeneration m_generation{kSurfaceGenerationNone};
    };

    // Frame settlement outcomes for the exactly-once retirement below.
    // kRejected: definitely unsubmitted (validation, obsolescence, busy
    // admission) — nothing references this frame's writes, the credit
    // returns immediately. kSubmitted: the submit was accepted and
    // signalled a value — the credit travels with it until fence
    // completion. kSubmitFailed: the submit call failed AFTER the work may
    // already have executed, so the frame is quarantined (admission stops,
    // the credit is held, the value is never committed or reused) until
    // proven idle or confirmed loss.
    enum class FrameSettlement : u8 {
        kRejected,
        kSubmitted,
        kSubmitFailed,
    };

    // P2 lane snapshot: owned copies only (the title borrows packet
    // storage that outlives the call via the lane request). Lets the render
    // lane drive presentation without touching the live Window; the legacy
    // overloads snapshot the live Window on the calling lane instead.
    struct SurfaceWindowSnapshot {
        WindowHandle m_handle{};
        NativeWindowHandle m_native{};
        int2 m_framebuffer_size{zero_v};
        std::string_view m_title{};
    };

    class Renderer final {
    public:
        rhi::Format m_preferred_surface_format{rhi::Format::Undefined};
        u32 m_desired_image_count{3u};
        bool m_enable_vsync{true};

        [[nodiscard]] std::error_code initialize(IRhiService &rhi_service);

        [[nodiscard]] std::error_code shutdown();

        // Device-loss hook: GPU objects are gone, so nothing in flight is
        // worth waiting for. Drops pending retirements and credits without
        // polling, releases surfaces/queue, and parks the fence for a later
        // shutdown + initialize restart. Never blocks on the GPU. Call ONLY
        // after removal is positively confirmed (sentinel, device-loss code,
        // or platform-confirmed loss) — never as proof on a generic UNKNOWN
        // wait failure, which must retain instead (see shutdown()).
        void notifyDeviceLost() noexcept;

        // Retention hook for an UNKNOWN idle-wait failure at teardown: parks
        // admission immediately so no new frame is built while GPU state of
        // unknown completion is retained. The caller must then retain every
        // GPU object (no destroy) and return the wait error for process
        // exit. Never used on the steady-state path.
        void quarantineAdmission() noexcept;

        [[nodiscard]] std::error_code render(
            string_literal description, const rhi::RenderPassDesc &render_pass,
            std::initializer_list<DrawSubmission> draws);

        [[nodiscard]] std::error_code renderToTexture(
            rhi::ITexture &render_target, std::initializer_list<DrawSubmission> draws,
            const ColorAttachmentOps &options = {});

        [[nodiscard]] std::error_code renderAndPresent(
            const Window &window, std::initializer_list<SurfaceRenderPass> passes);

        // Generational overload: `identity` carries the packet's surface
        // claim. A generation mismatch against the live record retires the
        // frame before acquisition (no backend work, no credit held); a
        // missing record (first frame) accepts and assigns. A "no claim"
        // identity (kSurfaceGenerationNone) skips the check — the legacy
        // overload below stays safe for callers without a packet.
        [[nodiscard]] std::error_code renderAndPresent(
            const Window &window, std::initializer_list<SurfaceRenderPass> passes, SurfaceIdentity identity);

        // Lane overload: identical semantics from a snapshot (no live
        // Window read). Temporary pass descriptors stay synchronous to the
        // encode, as on every other path.
        [[nodiscard]] std::error_code renderAndPresent(
            const SurfaceWindowSnapshot &window, std::span<const SurfaceRenderPass> passes, SurfaceIdentity identity);

        [[nodiscard]] std::error_code waitOnHost();

        // B1' quiescence probe, separated from state release: classifies GPU
        // completion WITHOUT releasing anything (no fence drop, no drain, no
        // surface/queue touch, never an app-fence signal). Success authorizes
        // release; positively-confirmed removal authorizes release without
        // touching the device; any other failure is UNKNOWN (callers retain
        // everything and fail-stop at final teardown). Never blocks
        // admission by itself — the caller quarantines first.
        [[nodiscard]] GpuQuiescenceResult establishGpuQuiescence();

        // B1' determinism seam, TESTS ONLY: when set, quiescence reports
        // this error instead of touching the backend (a healthy fence with
        // an io_error here is UNKNOWN without removal marking).
        // Production code never sets this.
        void setInjectedQuiescenceFailureForTest(std::error_code error) noexcept;

        // B3' queue-gate access, TESTS ONLY: the production graphics queue
        // the Renderer submits through, so a wait-only gate submit blocks the
        // SAME queue (no second worker). Production code never calls this.
        [[nodiscard]] rhi::ICommandQueue *graphicsQueueForTest() const noexcept;

        // Host wait for one app-owned fence value (growth gates, tests,
        // readback callers). Completion-only: display/vsync behavior is
        // unchanged by this call.
        [[nodiscard]] std::error_code waitForFenceValue(FenceCompletionValue value);

        // Retirement sweep: poll completion, reclaim deferred releases, and
        // return credits for submissions the fence has passed. Best effort:
        // a failed poll keeps every credit and every deferred entry (no
        // completion is ever invented). A latched-terminal poll surfaces the
        // device-loss code (never capacity-busy); callers that must stop
        // admission recheck isTerminal() after sweeping. Called at every
        // frame boundary and by tests; shutdown drains unconditionally
        // instead.
        [[nodiscard]] std::error_code retireCompletedFrames();

        // Non-acquiring admission probe shared by producers: sweeps
        // completions, then reports the frame's fate WITHOUT taking a
        // credit. Success means the producer may build; capacity-busy
        // (device_or_resource_busy) means skip the frame without building
        // anything (busy never fails the app); quarantine
        // (state_not_recoverable) or terminal device loss
        // (no_such_device_or_address) means the pipeline is parked — the
        // producer must propagate, never suppress as a dropped frame. The
        // terminal check runs again AFTER the sweep, so a removal sentinel
        // observed mid-sweep still stops admission.
        [[nodiscard]] std::error_code checkFrameAdmission();

        // Owner-thread retirement frontier: the highest accepted submission
        // value whose entry has actually left m_unretired_submits via a
        // successful sweep (completion observed by the fence poll — never
        // manufactured from a failed poll, a hold seam, or a removal). Main
        // uses the published mirror of this value to release construction
        // reservations only after both the CPU settlement result and this
        // retire marker cover the frame. Tracks kFenceValueNone before the
        // first confirmed retirement; never invented.
        [[nodiscard]] FenceCompletionValue retirementFrontier() const noexcept {
            return m_retirement_frontier;
        }

        // Owner-thread settlement of the most recent submit call's fate
        // (kSubmitted / kSubmitFailed / kRejected). The inline/lane submit
        // path copies the discriminator into the posted RenderFrameResult so
        // a construction reservation is charged (quarantined) on ambiguous
        // failures and released on definite rejections. Off-thread
        // (affinity) rejects never touch this member — they report through
        // the returned operation_not_permitted alone, so an accepted fate
        // the owner stamped is never corrupted before it is read; the caller
        // classifies that error as definitely-rejected without consulting it.
        [[nodiscard]] FrameSettlement lastSubmitSettlement() const noexcept {
            return m_last_submit_settlement;
        }

        // Tests-only deterministic backend-retirement driver: waits for the
        // fence value the lane has confirmed and submits an empty command
        // buffer on the same queue so slang-rhi's retirement poll drops its
        // completed command buffers. This is the backend workaround that
        // replaces the production notifyDeviceLost kick; it must only run on
        // the Renderer's owner thread, after proven completion of the real
        // submissions, and must not touch the device semantics of a loss.
        void driveQueueRetirementForTest() noexcept;

        // Observability for Gate 1 (four clocks stay separate: queue depth
        // via outstandingFrameCount, host present via profiling scopes, GPU
        // completion via completedFenceValue, displayed cadence externally).
        [[nodiscard]] FenceCompletionValue lastSubmittedFenceValue() const noexcept;
        [[nodiscard]] FenceCompletionValue completedFenceValue() const noexcept;
        [[nodiscard]] u32 outstandingFrameCount() const noexcept;

        // Current surface generation for a window handle, or
        // kSurfaceGenerationNone when no record exists (first frame).
        [[nodiscard]] SurfaceGeneration surfaceGenerationFor(WindowHandle window_handle) const noexcept;

        // Pre-acquisition obsolescence probe for producers: true when a
        // record exists and the packet's generation no longer matches it.
        [[nodiscard]] bool isSurfaceWorkObsolete(SurfaceIdentity identity) const noexcept;

        [[nodiscard]] std::error_code destroyWindowSurface(const Window &window);

        // Lane overload: destroys by handle without touching the live
        // Window (same affinity rules as every other Renderer call).
        [[nodiscard]] std::error_code destroyWindowSurface(WindowHandle window_handle);

        // Gate 1 determinism seam, TESTS ONLY: when set, the next submit
        // behaves as a submit that may already have executed (device-
        // removed class) — no value is committed and the frame quarantines.
        // Production code never sets this. Public solely so the
        // frame-fence tests can drive the quarantine path; behavior is
        // unchanged.
        void setInjectedSubmitFailure(bool fail) noexcept;

        // Gate 1 delayed-completion seam, TESTS ONLY: when held, the
        // frame-boundary sweep is suppressed so real submits stay
        // outstanding (credits held, deferred entries retained) until the
        // test releases the hold — proving two TRULY PENDING submissions via
        // real fence values. Production code never sets this.
        void setTestHoldSweep(bool hold) noexcept;

    private:
        // Per-surface cache of a pass-shape signature, one slot per cacheable
        // depth policy: slot 0 = none, slot 1 = renderer_owned. The 3D pass
        // (renderer_owned depth) and the UI pass (no depth) are different
        // shapes, so each policy owns its entry — one signature is never stored
        // across mixed passes. External depth and any pass carrying extra color
        // attachments is caller-shaped and is never cached.
        //
        // Each entry holds the signature inputs ONLY — (color formats, sample
        // count, depth format) — plus the extent it was computed for. Future
        // per-frame jitter must NEVER enter this signature: jitter lives in
        // frame constants (TSR direction), never in the pipeline key, or every
        // frame would compare unequal and wipe the cached variants.
        //
        // Those inputs derive from the surface config and the depth texture, so
        // resizeWindowSurface_ is the only place that recomputes them; it clears
        // both slots before reconfiguring and reseeds them on success, and the
        // stored extent lets a per-frame hit be rejected outright when the
        // surface no longer matches.
        struct SurfaceSignatureCache final {
            ESurfaceDepthPolicy m_depth_policy{ESurfaceDepthPolicy::none};
            RenderPipelineSignature m_signature{};
            int2 m_extent{zero_v};
            bool m_valid{false};
        };

        struct SurfaceRecord final {
            rhi::ComPtr<rhi::ISurface> m_surface{};
            rhi::ComPtr<rhi::ITexture> m_depth_texture{};
            rhi::ComPtr<rhi::ITextureView> m_depth_view{};
            int2 m_extent{zero_v};
            std::array<SurfaceSignatureCache, 2u> m_signatures{};
            // Generational lifetime: bumped on create/resize/unconfigure so
            // a packet built for an older shape is rejected before
            // acquisition instead of encoding against a stale surface.
            SurfaceGeneration m_generation{kSurfaceGenerationNone};
            bool m_configured: 1 {false};
        };

        // Surface-hoisted signature resolution for one phase-1 pass. The
        // attachments are still validated on every frame — the fail-closed
        // kMaxColorFormats check stays on the per-frame path, because the cache
        // holds the signature VALUE and never a "validated" verdict — while a
        // steady-state frame reuses the entry resizeWindowSurface_ recomputed
        // for the current surface shape.
        [[nodiscard]] std::error_code surfaceSignature_(
            SurfaceRecord &record, const SurfaceRenderPass &surface_pass, const rhi::RenderPassDesc &render_pass,
            RenderPipelineSignature &out_signature, int2 &out_target_extent);

        // Reseeds every cacheable per-pass signature entry from the inputs that
        // define it: the configured swapchain format, the backbuffer sample
        // count, and the depth texture's format. Called only from
        // resizeWindowSurface_ — the one place the surface shape changes.
        void recomputeSurfaceSignatures_(SurfaceRecord &record);

        [[nodiscard]] std::error_code createWindowSurface_(
            const SurfaceWindowSnapshot &window, SurfaceRecord **out_record);

        [[nodiscard]] std::error_code resizeWindowSurface_(SurfaceRecord &record, const int2 &new_extent);

        [[nodiscard]] std::error_code createSurfaceDepth_(
            const int2 &extent, rhi::ComPtr<rhi::ITexture> &out_texture,
            rhi::ComPtr<rhi::ITextureView> &out_view) const;

        [[nodiscard]] std::error_code renderDraws_(
            string_literal description, const rhi::RenderPassDesc &render_pass,
            const std::span<const DrawSubmission> draws);

        // One SUBMIT through the app-owned fence: signals nextValue() after
        // execute on the graphics queue and commits only on success. A failed
        // submit consumes no value and leaves no pending completion behind
        // (failed submits stall only the internal retirement, never the
        // app-owned monotonic sequence).
        [[nodiscard]] Expected<FenceCompletionValue> submitAndSignal_(rhi::ICommandBuffer *command_buffer);

        // Nonblocking admission shared by every submission path: a
        // quarantined submit failure or a terminal fence fails closed
        // before any construction; otherwise sweep completions first so a
        // finished frame never squats a credit, then take one. Quarantine
        // reports state_not_recoverable and terminal reports
        // no_such_device_or_address — both stay distinct from the
        // capacity-busy code all the way to teardown/recovery.
        [[nodiscard]] std::error_code admitFrame_();

        // Frame-boundary completion sweep shared by every submission path:
        // poll the fence (keeping prior completion on query failure),
        // reclaim deferred releases at or below completion, and return
        // credits for unretired submissions the fence has passed. Exactly one
        // credit return per submitted value; values never submitted never
        // enter the unretired set.
        void sweepCompleted_() noexcept;

        // Exactly-once per-frame settlement: a submitted value joins the
        // unretired set (credit returns when the fence passes it); a
        // rejected frame returns its credit immediately (definitely
        // unsubmitted); a failed submit quarantines the frame — admission
        // stops and the credit is held until proven idle (shutdown wait)
        // or confirmed loss (abandon). Every admission path calls this
        // exactly once, including rejections and failures.
        void retireFrame_(FenceCompletionValue submitted_value, FrameSettlement settlement) noexcept;

        [[nodiscard]] std::error_code renderAndPresentImpl_(
            const SurfaceWindowSnapshot &window, std::span<const SurfaceRenderPass> passes, SurfaceIdentity identity);

        [[nodiscard]] std::error_code destroyWindowSurface_(WindowHandle window_handle);

        [[nodiscard]] bool onRenderThread_() const noexcept;

        FlatMap<WindowHandle, SurfaceRecord> m_surfaces{};
        // Surface-generation epoch: every create/resize/unconfigure mints a
        // fresh nonzero generation from this counter. Never reused, never
        // reset except by process restart.
        SurfaceGeneration m_surface_epoch{kSurfaceGenerationNone};
        rhi::ComPtr<rhi::ICommandQueue> m_graphics_queue{};
        // The one app-owned monotonic fence: one signalled value per frame
        // SUBMIT, never an internal queue ID.
        FrameFence m_frame_fence{};
        // Two frame credits bounding submitted-but-uncompleted frames.
        // Single CPU lane, so admission never blocks the platform pump:
        // the Editor probes eligibility BEFORE building its packet (a full
        // ledger skips the frame without constructing anything) and maps a
        // busy/quarantined admission inside the Renderer to a dropped
        // frame — busy never fails the app.
        FrameCreditLedger m_credits{};
        // Submitted values awaiting fence completion; each leaves exactly
        // once, carrying its credit back with it.
        Array<FenceCompletionValue> m_unretired_submits{};
        // Advanced ONLY when an entry above actually leaves the set (its
        // submission completed, observed through a successful fence poll).
        // Hold-sweep, failed polls, and removal leave it at its last
        // proven value — retirement is never manufactured from absence.
        FenceCompletionValue m_retirement_frontier{kFenceValueNone};
        // Settlement of this submit call, for posting on the frame result.
        FrameSettlement m_last_submit_settlement{FrameSettlement::kRejected};
        // Submit-failure quarantine: a failed submit may have executed, so
        // admission stops and the failed frame's credit is held (never
        // reused, never returned early) until shutdown proves idle or
        // notifyDeviceLost confirms the loss. Cleared by both.
        bool m_submit_quarantine{false};
        // B1' quiescence determinism seam (tests only): a set error is
        // reported by establishGpuQuiescence instead of the backend wait.
        std::error_code m_injected_quiescence_failure{};
        // Gate 1 determinism seam: injected possibly-executed submit
        // failure (tests only; production never sets it).
        bool m_injected_submit_failure{false};
        // Gate 1 delayed-completion seam (tests only): when held,
        // sweepCompleted_ is suppressed so real submits stay outstanding.
        // Production never sets it.
        bool m_test_hold_sweep{false};
        // Set once shutdown() released the fence/queue/service: late sweeps
        // from the lane drain then report clean instead of not_connected.
        bool m_shutdown_complete{false};
        // Renderer-owned deferred releases (surface-adjacent; pass-owned
        // objects retire through the passes' own queues).
        DeferredReleaseQueue m_deferred{};
        safe_ptr<IRhiService> m_rhi_service{};
        std::thread::id m_owner{};
    };
}
