module engine.app;

import :frame.packet;

import std;

namespace pP {
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
