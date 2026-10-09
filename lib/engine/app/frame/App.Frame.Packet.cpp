module engine.app;

import :frame.packet;

import std;

namespace pP {
    namespace {
        // Lane/owner-thread side channel: capture-only heap-backed storage
        // (2,097,152 events x 64B = 128MiB) preallocated once before the
        // first record, guarded by a lock-free spin flag (record/drain stay
        // noexcept and allocation-free from any thread). Drained by main only
        // after the lane worker is proven joined. Heap-backed by design: an
        // inline array of this budget would bloat the binary's static storage
        // and any stack instance; the pointer stays pointer-sized. Bounded:
        // drops past capacity count toward overflow, reported at the single
        // exit-time export.
        std::atomic_flag g_lane_telemetry_lock{};
        std::unique_ptr<FrameTelemetryEvent[]> g_lane_telemetry_events{
            new (std::nothrow) FrameTelemetryEvent[kFrameTelemetryLaneCapacity]{}};
        u32 g_lane_telemetry_count{0u};
        u32 g_lane_telemetry_dropped{0u};

        void lockLaneTelemetry_() noexcept {
            while (g_lane_telemetry_lock.test_and_set(std::memory_order_acquire)) {
            }
        }

        void unlockLaneTelemetry_() noexcept {
            g_lane_telemetry_lock.clear(std::memory_order_release);
        }
    }

    FrameTelemetryBuffer &frameTelemetryMainBuffer() noexcept {
        thread_local FrameTelemetryBuffer buffer{};
        return buffer;
    }

    u32 frameTelemetryMainPending() noexcept {
        return frameTelemetryMainBuffer().size();
    }

    u32 frameTelemetryMainDropped() noexcept {
        return frameTelemetryMainBuffer().overflow();
    }

    void frameTelemetryRecordLane(const FrameTelemetryEvent &event) noexcept {
        lockLaneTelemetry_();
        if (g_lane_telemetry_events == nullptr or g_lane_telemetry_count >= kFrameTelemetryLaneCapacity) {
            ++g_lane_telemetry_dropped;
        } else {
            g_lane_telemetry_events[g_lane_telemetry_count] = event;
            ++g_lane_telemetry_count;
        }
        unlockLaneTelemetry_();
    }

    u32 frameTelemetryLanePending() noexcept {
        lockLaneTelemetry_();
        const u32 pending = g_lane_telemetry_count;
        unlockLaneTelemetry_();
        return pending;
    }

    u32 frameTelemetryLaneDropped() noexcept {
        lockLaneTelemetry_();
        const u32 dropped = g_lane_telemetry_dropped;
        unlockLaneTelemetry_();
        return dropped;
    }

    u32 frameTelemetryMergeLaneIntoMain() noexcept {
        FrameTelemetryBuffer &target = frameTelemetryMainBuffer();
        u32 merged = 0u;
        lockLaneTelemetry_();
        if (g_lane_telemetry_events != nullptr) {
            for (u32 index = 0u; index < g_lane_telemetry_count; ++index) {
                const u32 before = target.size();
                target.record(g_lane_telemetry_events[index]);
                if (target.size() != before) {
                    ++merged;
                }
            }
        }
        g_lane_telemetry_count = 0u;
        unlockLaneTelemetry_();
        return merged;
    }

    void dumpFrameTelemetryMain() noexcept {
        FrameTelemetryBuffer &buffer = frameTelemetryMainBuffer();
        try {
            std::printf(
                "[frame-telemetry clock=%s] main_events=%u main_dropped=%u lane_pending=%u lane_dropped=%u\n",
                frameTelemetryClockName(), static_cast<unsigned>(buffer.size()),
                static_cast<unsigned>(buffer.overflow()), static_cast<unsigned>(frameTelemetryLanePending()),
                static_cast<unsigned>(frameTelemetryLaneDropped()));
            std::println("tl_kind,tl_epoch,tl_frame,cpu_tick_ns,gpu_fence,detail_a,detail_b,host_outcome,thread");
            for (u32 index = 0u; index < buffer.size(); ++index) {
                const FrameTelemetryEvent &event = buffer.m_events[index];
                std::printf(
                    "%s,%llu,%llu,%lld,%llu,%lld,%lld,%d,%u\n", frameTelemetryEventName(event.m_kind),
                    static_cast<unsigned long long>(event.m_epoch), static_cast<unsigned long long>(event.m_frame_id),
                    static_cast<long long>(event.m_cpu_tick_ns),
                    static_cast<unsigned long long>(event.m_gpu_fence_value), static_cast<long long>(event.m_detail_a),
                    static_cast<long long>(event.m_detail_b), event.m_host_outcome,
                    static_cast<unsigned>(event.m_thread_tag));
            }
        } catch (...) {
            // Diagnostics only, including throwing std::println: lost output cannot alter settlement or shutdown.
        }
        buffer.clear();
    }

    bool hasRenderTarget(const FramePacket &packet) noexcept {
        // Derived from the owned framebuffer size alone: zero-size /
        // minimized surfaces are an explicit non-rendering outcome (P1
        // generational-surface precursor). No stored flag, so no desync.
        return packet.m_framebuffer_size.x > 0 and packet.m_framebuffer_size.y > 0;
    }

    FramePacket makeFramePacket(
        const u64 frame_id, const TimeSpan delta_time, const TimeSpan total_elapsed, const CameraSnapshot &camera,
        const Viewport &viewport, const WindowViewport::ViewportRevision viewport_revision, const Window &window) {
        FramePacket packet{};
        packet.m_frame_id = frame_id;
        packet.m_delta_time = delta_time;
        packet.m_total_elapsed = total_elapsed;
        packet.m_camera = camera;
        packet.m_viewport = viewport;
        packet.m_viewport_revision = viewport_revision;
        packet.m_window_title = window.m_title;
        packet.m_framebuffer_size = window.m_framebuffer_size;
        // Surface identity is authoritative at capture; the generation is
        // filled by the producer (Editor::update queries the Renderer, or
        // the lane generation mirror), defaulting to "no claim" for
        // producers without a surface yet. The native handle is snapshotted
        // so the consume lane can create the surface without touching the
        // live Window.
        packet.m_surface_identity = window.m_handle;
        packet.m_surface_native = window.m_native;
        return packet;
    }
}
