module;
#include "pP/Macros.h"
export module engine.app:renderer.frame_fence;

import engine.core;
import engine.rhi;
import std;

export namespace pP {
    // ------------------------------------------------------------------
    // P1 fence-governed GPU reuse: app-owned monotonic completion fence
    // ------------------------------------------------------------------
    // The sole GPU-completion primitive for frame reuse. One IFence is owned
    // by the Renderer (created at initialize, released at shutdown); every
    // frame SUBMIT signals an app-owned monotonic value through SubmitDesc
    // signalFences (after-execute placement on the single graphics queue).
    // Internal queue IDs are never used: a failed submit consumes NO value,
    // so a value always names a submission that was actually accepted.
    //
    // Resources are associated with SUBMISSION completion, never CPU frame
    // modulo or swapchain image count. Completion is GPU-only: there is no
    // display signal — present-call duration and surface lifetime are
    // governed by their own backend contract, not by this fence.
    //
    // Two-credit bound (P1, still one CPU lane): at most
    // kMaxOutstandingFrames submitted-but-uncompleted frames. The bound is
    // enabled ONLY because every mutable resource below has an explicit
    // last-use rule (see renderer/codemap.md R1-R25):
    //   Grid/Triangle payload rings (3 slots) + indirect args keyed by
    //     per-slot completion values (shared cursor covered once);
    //   UI FrameResources[2] keyed by completion (zero slack: 2 slots for
    //     2 credits, so the second outstanding frame may have to wait);
    //   per-frame/per-slot root shader objects (no persistent-root mutation);
    //   deferred release for pipelines/variants/UI growth (growth WAITS);
    //   atlas keeps the existing global-idle wait as first implementation
    //     (it serializes dirty frames; no overlap is claimed).

    using FenceCompletionValue = u64;

    inline constexpr FenceCompletionValue kFenceValueNone{0u};

    // Two frame credits: building/queued/recording/GPU-pending frames
    // collectively consume at most two. Pairwise overlap only — three
    // distinct in-flight frames would need three credits.
    inline constexpr u32 kMaxOutstandingFrames{2u};

    // Grid/Triangle ring depth. 3 slots > 2 credits, so the slot a frame
    // rotates onto is complete without waiting whenever every submission
    // retired in order; the wait fallback below covers the rest.
    inline constexpr u32 kFrameRingSlots{3u};

    // Generational surface identity: a packet names the surface it was built
    // for. kSurfaceGenerationNone means "no generation claimed" (first frame
    // before any surface exists); a record always carries a nonzero
    // generation once configured.
    using SurfaceGeneration = u64;

    inline constexpr SurfaceGeneration kSurfaceGenerationNone{0u};

    // Slot readiness: a ring slot written by the submission that signalled
    // `slot_value` is reusable once the fence has passed that value. A slot
    // never written (kFenceValueNone) is trivially reusable.
    [[nodiscard]] constexpr bool slotFenceComplete(
        const FenceCompletionValue completed, const FenceCompletionValue slot_value) noexcept {
        return slot_value <= completed;
    }

    // Ring-slot selection over per-slot last-use completion values. Scans
    // forward from `cursor` for the first complete slot; when every slot is
    // still in flight (only possible past the credit bound, or on
    // artificially delayed completion) returns the next slot anyway and the
    // caller MUST wait for its value before writing. Pure CPU, so the
    // no-early-overwrite rule is unit-testable with fake completion values.
    [[nodiscard]] u32 selectRingSlot(
        FenceCompletionValue completed, std::span<const FenceCompletionValue> slot_values, u32 cursor) noexcept;

    // Growth gate: replacing a whole ring (or any buffer a submitted frame
    // may still read) must wait for the YOUNGEST in-flight use, i.e. the
    // maximum over the slot values — never per-slot, never unfenced.
    [[nodiscard]] FenceCompletionValue maxSlotValue(std::span<const FenceCompletionValue> slot_values) noexcept;

    // Pre-acquisition rejection: obsolete work (a packet built for an older
    // surface generation) is retired WITHOUT touching the backend. No record
    // yet (first frame, surface created lazily) never rejects — creation
    // accepts the packet and assigns the generation.
    [[nodiscard]] constexpr bool isSurfaceWorkObsolete(
        const bool has_record, const SurfaceGeneration packet_generation,
        const SurfaceGeneration record_generation) noexcept {
        return has_record and packet_generation != record_generation;
    }

    // ------------------------------------------------------------------
    // GPU quiescence disposition (B1' fail-stop): the three readings of a
    // final idle wait. Only proven-idle or positively-confirmed removal
    // authorizes state release; UNKNOWN authorizes nothing and fail-stops
    // the process at final teardown (see Application::prepareGpuTeardown_).
    // A blocked driver call below still needs an EXTERNAL supervisor
    // (watchdog/kill): no in-process wait can preempt a hung driver.
    // ------------------------------------------------------------------

    enum class GpuQuiescence : u8 {
        // A wait returned success (or a poll observed the value): every
        // submitted frame completed; release is authorized.
        kIdleProven,
        // Positively-confirmed removal (no_such_device_or_address, or a
        // latched-terminal fence): completion can never arrive; release
        // WITHOUT touching the device is authorized.
        kRemovalConfirmed,
        // Any other wait/query failure: nothing proven either way; release
        // is NOT authorized — retain everything and fail-stop at teardown.
        kUnknown,
    };

    struct GpuQuiescenceResult {
        GpuQuiescence m_disposition{GpuQuiescence::kUnknown};
        std::error_code m_error{};
    };

    // Final-teardown failure handler for UNKNOWN GPU completion. Invoked
    // exactly once when release cannot be authorized; must be noexcept, must
    // not throw, must not depend on the logger drain. The caller terminates
    // the process even if the handler returns. Production default (nullptr)
    // terminates directly with the fatal status below.
    using GpuTeardownFailureHandler = void (*)(std::error_code quiescence_error) noexcept;

    // Explicit fatal exit status for UNKNOWN completion at final teardown:
    // nonzero, distinctive, preserved through spawnAndWait on every backend.
    inline constexpr int kUnknownGpuCompletionFatalExit{0xB1};

    // ------------------------------------------------------------------
    // frame credit ledger: at most kMaxOutstandingFrames uncompleted frames
    // ------------------------------------------------------------------
    // Pure CPU accounting. Admission acquires; retirement releases exactly
    // once per admitted frame, including exceptional paths (a frame that
    // never submitted releases immediately; a submitted frame releases when
    // its fence value completes). Double release fails closed; acquisition
    // past the bound fails closed with device_or_resource_busy.

    class FrameCreditLedger {
    public:
        [[nodiscard]] std::error_code acquire() noexcept;

        [[nodiscard]] std::error_code release() noexcept;

        // Device-loss/shutdown escape: nothing in flight is worth waiting
        // for once the GPU objects are gone; drops every outstanding credit
        // without completing them. Never used on the steady-state path.
        void reset() noexcept;

        [[nodiscard]] u32 outstanding() const noexcept {
            return m_outstanding;
        }

        [[nodiscard]] static constexpr u32 capacity() noexcept {
            return kMaxOutstandingFrames;
        }

    private:
        u32 m_outstanding{0u};
    };

    // ------------------------------------------------------------------
    // deferred release: retire GPU objects only past their last-use value
    // ------------------------------------------------------------------
    // Pipelines, variant roots, and replaced UI buffers referenced by
    // in-flight submissions cannot be dropped at replacement time. They are
    // queued with the fence value of the latest submission that may
    // reference them and destroyed (ComPtr release is CPU-side refcounting,
    // safe without a device) once completion passes that value. Shutdown and
    // device loss drain unconditionally — the objects are dead either way.

    class DeferredReleaseQueue {
    public:
        void defer(FenceCompletionValue last_use_value, std23::move_only_function<void()> release) noexcept;

        // Destroys every entry at or below `completed`, in deferral order.
        // Returns the reclaimed entry count.
        [[nodiscard]] u32 reclaim(FenceCompletionValue completed) noexcept;

        // Unconditional drain for shutdown/device-loss. Returns the count.
        [[nodiscard]] u32 drainAll() noexcept;

        [[nodiscard]] u32 pendingCount() const noexcept;

    private:
        struct DeferredEntry {
            FenceCompletionValue m_last_use{kFenceValueNone};
            std23::move_only_function<void()> m_release{};
        };

        Array<DeferredEntry> m_entries{};
    };

    // ------------------------------------------------------------------
    // app-owned monotonic fence around a single IFence
    // ------------------------------------------------------------------
    // Render-thread confined (affinity is enforced by the Renderer, which
    // owns this object). The device pointer is a non-owning view: the RHI
    // service outlives the Renderer by construction (teardown is the inverse
    // of setup), and shutdown clears it.
    //
    // Completion-state split (G1-3 retention policy — the three outcomes
    // below are the ONLY readings; a host-wait failure is never treated as
    // confirmed loss):
    //   proven-idle:        a wait that returned success (or a poll that
    //                        observes the value) proves completion at or past
    //                        that value; only proven-idle state is normally
    //                        released or reused.
    //   positively-confirmed removal: the UINT64_MAX sentinel observed via
    //                        poll, or a wait/poll that fails closed with
    //                        no_such_device_or_address — completion can never
    //                        arrive; latches isTerminal(), and every later
    //                        poll/wait/admission fails closed with the
    //                        device-loss code (never capacity-busy).
    //                        abandon() is called ONLY on this path (teardown
    //                        after positively-confirmed removal, or an
    //                        explicit notifyDeviceLost after the platform
    //                        confirmed removal). abandon() proves nothing by
    //                        itself and is never used as proof.
    //   UNKNOWN completion: any other wait or query FAILURE (timeout,
    //                        transport, driver error). Nothing is proven
    //                        either way, so the previous high-water mark
    //                        stands, every credit and every deferred entry is
    //                        retained, no value is reused, admission is
    //                        parked (quarantine), and the error propagates —
    //                        the frame is quarantined, never normally
    //                        released. At shutdown an UNKNOWN idle wait
    //                        RETAINS every GPU object (fence, deferred queue,
    //                        surfaces, queue, pass/UI resources held by the
    //                        caller) and returns the error for process exit —
    //                        never a silent release, never abandon() as proof.
    //                        The OS reclaims retained objects at process exit;
    //                        only proven idle or positively-confirmed removal
    //                        releases them early.
    // Teardown retention: at shutdown there is no later recovery point, so
    // an UNKNOWN idle wait parks admission, retains the fence alive (no
    // abandon, no drain, no reset — a later process-exit reclaims it), and
    // returns the wait error. Mid-frame UNKNOWN never escalates: the error
    // propagates and the next frame retries against retained state. Only a
    // positively-confirmed removal escalates to abandon + release-without-
    // touching-the-device.

    class FrameFence {
    public:
        [[nodiscard]] std::error_code initialize(rhi::IDevice &device);

        // Best effort, retain-first-error: waits for the last submission only
        // when a queue submission actually happened, then drops the fence.
        // Uninitialized shutdown is a no-op success (idempotent teardown).
        // A terminal fence (device-loss sentinel observed) skips the wait —
        // there is nothing worth waiting for — and a failed wait RETAINS
        // (UNKNOWN completion retained at teardown: the fence stays alive,
        // no abandon, no drain, no reset — process exit reclaims it) while
        // still reporting the wait error, EXCEPT a positively-confirmed
        // removal (no_such_device_or_address) which abandons wait-free.
        // Use abandon() when removal is positively confirmed and no wait is
        // allowed at all; never as proof of completion.
        [[nodiscard]] std::error_code shutdown();

        // No-wait abandonment for positively-confirmed removal ONLY: drops
        // the fence and device view without polling or waiting, and latches
        // the terminal state so later pollCompleted/waitFor fail closed
        // instead of inventing completion. Call ONLY after removal is
        // positively confirmed (sentinel observed, device-loss code, or an
        // explicit platform-confirmed notifyDeviceLost) — never as proof on
        // a generic UNKNOWN wait failure, which must retain instead.
        // notifyDeviceLost paths must call this, never
        // shutdown() (which is allowed to wait).
        void abandon() noexcept;

        // Terminal device-loss latch: set when completion can never arrive
        // (removal sentinel observed, query failed terminally, or abandon()
        // called). Admission must stop once this is set; only
        // shutdown + initialize restarts. Terminal failures surface as
        // no_such_device_or_address — always distinct from the
        // capacity-busy code, all the way to teardown/recovery.
        [[nodiscard]] bool isTerminal() const noexcept {
            return m_terminal;
        }

        // Value the NEXT successful submit will signal. Not consumed until
        // commitSubmitted: a failed submit burns nothing (the internal queue
        // ID may burn, the app-owned value never does).
        [[nodiscard]] FenceCompletionValue nextValue() const noexcept {
            return m_next_value;
        }

        void commitSubmitted(FenceCompletionValue value) noexcept;

        // Best-effort poll: refreshes the known completion from
        // getCurrentValue. On query failure the previous completion stands —
        // completion is never invented. A UINT64_MAX read is the
        // device-removal sentinel, not a value: it latches the terminal
        // state and reports the loss instead of publishing completion.
        // A latched-terminal poll reports no_such_device_or_address (the
        // confirmed-removal code); a never-initialized fence reports
        // not_connected — the two never share a code.
        [[nodiscard]] Expected<FenceCompletionValue> pollCompleted() noexcept;

        // Host wait for one value (growth gates, slot waits). Infinite
        // timeout by default, matching the existing waitOnHost bluntness;
        // the GPU-pending bound keeps it bounded in practice. Fails closed
        // immediately on a terminal fence or a value that can never
        // complete. A FAILED wait is UNKNOWN completion (never confirmed
        // loss): the high-water mark stands, nothing is released or reused,
        // and the error propagates — see the escalation policy above. On
        // success the high-water mark refreshes through the poll path
        // (observed, never assumed), and a post-wait poll failure (incl. the
        // sentinel) propagates INSTEAD of success, so a wait that raced
        // removal never reports completion that never arrives.
        [[nodiscard]] std::error_code waitFor(FenceCompletionValue value, u64 timeout_ns = rhi::kTimeoutInfinite);

        [[nodiscard]] FenceCompletionValue lastSubmitted() const noexcept {
            return m_last_submitted;
        }

        [[nodiscard]] FenceCompletionValue completed() const noexcept {
            return m_completed;
        }

        [[nodiscard]] rhi::IFence *fence() const noexcept {
            return m_fence.get();
        }

        [[nodiscard]] bool isInitialized() const noexcept {
            return m_fence != nullptr;
        }

    private:
        rhi::ComPtr<rhi::IFence> m_fence{};
        rhi::IDevice *m_device{nullptr};
        FenceCompletionValue m_next_value{1u};
        FenceCompletionValue m_last_submitted{kFenceValueNone};
        FenceCompletionValue m_completed{kFenceValueNone};
        bool m_terminal{false};
    };
} // namespace pP
