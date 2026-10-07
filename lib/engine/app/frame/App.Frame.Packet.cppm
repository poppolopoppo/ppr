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
}
