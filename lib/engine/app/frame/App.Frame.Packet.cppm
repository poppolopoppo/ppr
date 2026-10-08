module;

export module engine.app:frame.packet;

import :renderer.frame_fence;
import :scene.camera;
import :window.handle;
import :window.viewport;

import engine.core;
import engine.math;
import std;

export namespace pP {
    // ------------------------------------------------------------------
    // P0 serial-explicit owned frame packet: values and owned storage
    // only. Constructed on the client thread at the end of update(),
    // consumed on the same thread by render(). No producer ScratchPad,
    // borrowed upload spans, initializer-list backing, or function_ref
    // escapes into (or out of) the packet — the render lane of P2 will
    // move this same packet across threads instead of rebuilding it.
    // ------------------------------------------------------------------

    struct FramePacket {
        u64 m_frame_id{};

        TimeSpan m_delta_time{};
        TimeSpan m_total_elapsed{};

        // Owned camera copy (revision included); never a live Camera view.
        CameraSnapshot m_camera{};

        // Owned viewport copy + revision at capture time.
        Viewport m_viewport{};
        WindowViewport::ViewportRevision m_viewport_revision{};

        // Copied window metadata (render metadata only — there are no
        // live title producers); never a live Window reference. The
        // render-target verdict derives from m_framebuffer_size alone
        // (see hasRenderTarget): no stored flag can desync from it.
        std::string m_window_title{};
        int2 m_framebuffer_size{};

        // Generational surface claim: the surface identity this packet was
        // built for, plus the Renderer surface generation observed at
        // capture time. The consume path rejects a packet whose generation
        // no longer matches the live record BEFORE acquisition; the first
        // frame (kSurfaceGenerationNone, no record yet) always accepts.
        WindowHandle m_surface_identity{};
        SurfaceGeneration m_surface_generation{kSurfaceGenerationNone};
        // Owned native handle snapshot for surface creation on the consume
        // lane: the platform window outlives every in-flight packet by the
        // P2 teardown order (windows die on main after the lane joins), so
        // the consume lane never touches the live Window for this.
        NativeWindowHandle m_surface_native{};
    };

    [[nodiscard]] bool hasRenderTarget(const FramePacket &packet) noexcept;

    [[nodiscard]] FramePacket makeFramePacket(
        u64 frame_id, TimeSpan delta_time, TimeSpan total_elapsed, const CameraSnapshot &camera,
        const Viewport &viewport, WindowViewport::ViewportRevision viewport_revision, const Window &window);

    // ------------------------------------------------------------------
    // Phase 3 entry measurement-only telemetry (ora-25). Side-channel
    // event lane: correlated timestamp records keyed by reservation
    // epoch + frame id. FramePacket semantics are untouched (it keeps
    // only latched sim timing); cross-thread correlation rides these
    // side-channel events, never packet fields.
    //
    // Clock discipline: every m_cpu_tick_ns stamps ONE monotonic CPU
    // clock (std::chrono::steady_clock, nanosecond ticks). The export
    // header records its frequency/origin (see
    // frameTelemetryClockName). Transport/host/GPU/display clocks stay
    // distinct by field naming: GPU progress travels ONLY as
    // m_gpu_fence_value, host outcomes ONLY as m_host_outcome, and
    // display cadence is NEVER measured here (PresentMon owns it).
    //
    // Buffer discipline: bounded preallocated per-thread storage, no
    // per-frame I/O. The main thread owns a thread_local buffer;
    // lane/owner-thread probes record into a spinlock-guarded lane
    // buffer that main drains/merges after the lane joins. record()
    // never allocates, never throws, never alters control flow;
    // overflow is counted and reported at export, never silently shed.
    // The sink is explicit at every probe site (buffer + clock passed
    // by the caller: Application threads m_application_clock.now()
    // through; platform reads the application-owned TimerManager; the
    // lane uses the same steady_clock backing that TimerManager
    // defaults to), never an implicit singleton read inside logic.
    // ------------------------------------------------------------------

    enum class EFrameTelemetryEvent : u8 {
        clock_deadline_intended = 0,
        clock_tick_actual,
        clock_uncapped,
        admission_credit,
        client_phase_begin,
        client_phase_end,
        frame_skipped,
        platform_device_poll,
        platform_collect,
        platform_wait_begin,
        platform_wait_end,
        input_consumed,
        packet_snapshot,
        lane_enqueue,
        lane_enqueue_dropped,
        lane_dequeue,
        lane_execute_begin,
        lane_execute_end,
        renderer_acquire,
        renderer_submit,
        renderer_present,
        fence_retired,
    };

    struct FrameTelemetryEvent {
        EFrameTelemetryEvent m_kind{EFrameTelemetryEvent::clock_tick_actual};
        u64 m_epoch{0u};
        u64 m_frame_id{0u};
        i64 m_cpu_tick_ns{0}; // single monotonic CPU clock (steady_clock, ns)
        FenceCompletionValue m_gpu_fence_value{kFenceValueNone}; // GPU domain only, never a timestamp
        i64 m_detail_a{0}; // kind-specific (deadline delta us, dt us, credit outstanding, queue depth...)
        i64 m_detail_b{0}; // kind-specific (error value, submit outcome...)
        int m_host_outcome{0}; // host error value, 0 == success; distinct from GPU fence
        u8 m_thread_tag{0}; // 0 == main thread, 1 == lane/owner thread
    };

    // Capture budgets (NOT sampling): capture-only heap-backed buffers
    // preallocated once before the first record; record() never allocates.
    // sizeof(FrameTelemetryEvent) == 64B (1B kind + 7B pad + 6x8B
    // epoch/frame/cpu-tick/gpu-fence/detail-a/detail-b = 56B + 4B
    // host-outcome + 1B thread-tag + 3B tail pad = 64B), so main/merged
    // 4,194,304 events = 256MiB and lane 2,097,152 events = 128MiB
    // (384MiB total + ~48MiB ASan shadow at 1/8). Heap-backed by design:
    // a 256MiB inline/thread_local array would multiply across TLS users
    // and a 256MiB stack instance would overflow — the thread_local/stack
    // object stays pointer-sized while the events live on the heap.
    // Bounded: drops past capacity count toward overflow and are reported
    // at the single exit-time export.
    inline constexpr u32 kFrameTelemetryMainCapacity{4194304u};
    inline constexpr u32 kFrameTelemetryLaneCapacity{2097152u};

    static_assert(
        sizeof(FrameTelemetryEvent) == 64u, "FrameTelemetryEvent size changed: update capture budget comment");

    struct FrameTelemetryBuffer {
        static constexpr u32 kCapacity{kFrameTelemetryMainCapacity};
        std::unique_ptr<FrameTelemetryEvent[]> m_events{new (std::nothrow) FrameTelemetryEvent[kCapacity]{}};
        u32 m_count{0u};
        u32 m_dropped{0u};

        void record(const FrameTelemetryEvent &event) noexcept {
            if (m_events == nullptr or m_count >= kCapacity) {
                ++m_dropped;
                return;
            }
            m_events[m_count] = event;
            ++m_count;
        }

        [[nodiscard]] u32 size() const noexcept {
            return m_count;
        }

        [[nodiscard]] u32 overflow() const noexcept {
            return m_dropped;
        }

        void clear() noexcept {
            m_count = 0u;
            m_dropped = 0u;
        }
    };

    [[nodiscard]] inline i64 frameTelemetryCpuTickNs(const std::chrono::steady_clock::time_point tick) noexcept {
        return static_cast<i64>(std::chrono::duration_cast<std::chrono::nanoseconds>(tick.time_since_epoch()).count());
    }

    [[nodiscard]] inline const char *frameTelemetryClockName() noexcept {
        return "steady_clock monotonic CPU clock, ns ticks (1GHz nominal); origin = steady_clock epoch; "
               "GPU fence/host outcome/display cadence kept in distinct fields (display cadence not measured here)";
    }

    [[nodiscard]] inline const char *frameTelemetryEventName(const EFrameTelemetryEvent kind) noexcept {
        switch (kind) {
            case EFrameTelemetryEvent::clock_deadline_intended: return "clock_deadline_intended";
            case EFrameTelemetryEvent::clock_tick_actual: return "clock_tick_actual";
            case EFrameTelemetryEvent::clock_uncapped: return "clock_uncapped";
            case EFrameTelemetryEvent::admission_credit: return "admission_credit";
            case EFrameTelemetryEvent::client_phase_begin: return "client_phase_begin";
            case EFrameTelemetryEvent::client_phase_end: return "client_phase_end";
            case EFrameTelemetryEvent::frame_skipped: return "frame_skipped";
            case EFrameTelemetryEvent::platform_device_poll: return "platform_device_poll";
            case EFrameTelemetryEvent::platform_collect: return "platform_collect";
            case EFrameTelemetryEvent::platform_wait_begin: return "platform_wait_begin";
            case EFrameTelemetryEvent::platform_wait_end: return "platform_wait_end";
            case EFrameTelemetryEvent::input_consumed: return "input_consumed";
            case EFrameTelemetryEvent::packet_snapshot: return "packet_snapshot";
            case EFrameTelemetryEvent::lane_enqueue: return "lane_enqueue";
            case EFrameTelemetryEvent::lane_enqueue_dropped: return "lane_enqueue_dropped";
            case EFrameTelemetryEvent::lane_dequeue: return "lane_dequeue";
            case EFrameTelemetryEvent::lane_execute_begin: return "lane_execute_begin";
            case EFrameTelemetryEvent::lane_execute_end: return "lane_execute_end";
            case EFrameTelemetryEvent::renderer_acquire: return "renderer_acquire";
            case EFrameTelemetryEvent::renderer_submit: return "renderer_submit";
            case EFrameTelemetryEvent::renderer_present: return "renderer_present";
            case EFrameTelemetryEvent::fence_retired: return "fence_retired";
        }
        return "unknown";
    }

    // Per-thread main buffer (thread_local; the main thread's instance is
    // shared by every main-thread probe site in this process).
    [[nodiscard]] FrameTelemetryBuffer &frameTelemetryMainBuffer() noexcept;

    [[nodiscard]] u32 frameTelemetryMainPending() noexcept;

    [[nodiscard]] u32 frameTelemetryMainDropped() noexcept;

    // Lane/owner-thread record path (spinlock-guarded, any-thread safe,
    // noexcept, allocation-free). Drained/merged by main after join.
    void frameTelemetryRecordLane(const FrameTelemetryEvent &event) noexcept;

    [[nodiscard]] u32 frameTelemetryLanePending() noexcept;

    [[nodiscard]] u32 frameTelemetryLaneDropped() noexcept;

    // Merge lane events into the caller's (main) buffer; drops on a full
    // target count toward the target's overflow. Returns merged count.
    [[nodiscard]] u32 frameTelemetryMergeLaneIntoMain() noexcept;

    // Single exit-time export of the main buffer (bounded, once; never
    // per-frame). Prints the clock header plus one CSV line per event,
    // then clears. noexcept, allocation-free.
    void dumpFrameTelemetryMain() noexcept;
}
