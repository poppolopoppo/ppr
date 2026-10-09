# lib/engine/app/frame

## Responsibility

`engine.app:frame.packet` is the P0 serial-explicit owned frame packet: values and owned storage only, built on the
client thread after the camera update and consumed by render. It carries the frame id, latched
timing, the copied `CameraSnapshot` (revision included), the copied `Viewport` plus viewport revision, copied
window metadata (title string, framebuffer size, native-handle snapshot), and the generational surface claim
(`m_surface_identity` + `m_surface_generation`, P1). No producer `ScratchPad`, borrowed upload spans,
initializer-list backing, or `function_ref` escapes into or out of the packet.

`engine.app:frame.render_lane` is the P2 dedicated render worker (Application-owned thread + three bounded
`RawChannel` transports: frame requests forward, results back, independent control/wake). It executes an injected
executor per frame, sweeps fence completion on the lane, and settles queued work at exit. See Design below.

## Design

- `FramePacket` — plain struct of owned copies: `m_frame_id`, `m_delta_time`/`m_total_elapsed` (manager-latched),
  `m_camera`, `m_viewport` + `m_viewport_revision`, `m_window_title`, `m_framebuffer_size`,
  `m_surface_identity` (from the live handle at capture) + `m_surface_generation` (stamped by the producer from
  the Renderer generation or, on the lane, from the result-fed mirror; `kSurfaceGenerationNone` when no record
  exists yet) + `m_surface_native` (snapshot so the consume lane can create the surface without touching the live
  `Window`). The render-target verdict is derived from `m_framebuffer_size` alone (`hasRenderTarget`):
  zero-size/minimized is an explicit non-rendering success, and no stored flag can desync from the size.
- `makeFramePacket(...)` copies every field at capture time; `hasRenderTarget(...)` is pure. Both are defined in
  `App.Frame.Packet.cpp` (non-inline, non-template definitions live with the matching `.cpp`; the `.cppm` exports
  declarations only).
- Pass staging consumes the packet snapshot (`update(dt, packet.m_camera)`), never the live `Camera`, so a
  post-capture producer mutation cannot alter render preparation. `render()` consumes the staged packet single-use
  (fail closed when absent) and submits it (worker) or executes it (inline reference) the same frame.
- `RenderLane` — one fixed worker + `Channel`-free `RawChannel` triple (depths 2/4/8: frames/results/control).
  Forward requests carry `shared_ptr<const FramePacket>` + `DetachedUiDrawData` with `drop_if_full` (full maps to
  capacity-busy: drop the frame, never fail the app); results carry submit value/error/drop/font-ack/surface
  generation; control carries wake/exit/config (exit additionally latches an atomic, so a full queue never blocks
  exit signalling). Exactly one logical consumer per channel; the signalling path never uses
  `wait_if_full`/`flush`/`close`. The lane multiplexes both inbound queues with `select()` (wakeable), runs a
  synchronous handoff for teardown-time lane operations (pass-GPU release, Renderer shutdown, quiescence probes),
  and settles on exit (discards unsubmitted as dropped results, final sweep). Destructor stops + joins before the
  channels die. Thread rule: the lane runs only injected callbacks; its sole cross-thread signal is the wake hint
  (`PulseEvent` + `postEmptyEvent`).
- Ownership != affinity: content owners keep their objects (editor-owned passes, client-owned ImGui context,
  game-owned snapshots/translator); the lane is only the fixed execution affinity.
- Phase 4 surface transactions: `requestSurfaceReconfigure(RenderSurfaceRequest)` owns its value payload,
  returns busy/canceled on non-admission, and permits one outstanding transaction until its ack is drained.
  `setSurfaceExecutor` binds the lane callback before admission. Existing queued frames execute before the
  surface callback; there is no CPU barrier parking their executor. Backend present/idle calls can delay the ack.
  `RenderFrameResult::m_surface_result` is a distinct control completion on the existing reliable result
  transport/stash: **dispatch it before frame credit/staging accounting**, including in suspended-admission
  pump paths. It carries transaction ID, echoed handle/request generation, actual applied generation/state,
  original error and restoration error. Validate the echoed identity against the current pending candidate;
  only then update the mirror and resume admission. No control message consumes a frame credit.
  `drainQueuedFrames` closes surface admission and cancels/acknowledges queued controls before resource release;
  requestStop settles remaining controls, and join precedes storage destruction.

## Flow

1. `ApplicationEditor::update()` → camera `updateModel` → `makeFramePacket(...)` → pass `update(dt, packet.m_camera)`
   → scene/UI work.
2. `ApplicationEditor::render()` → take + clear the staged packet → `submitRenderWork_(packet)` → `hasRenderTarget`
   guard → `IUIService::buildDetachedUiFrame` on main (completes `ImGui::Render()`, publishes owned data) →
   enqueue to the lane (`drop_if_full` → bounded hint wait) or `executeRenderWork_` inline.
3. Lane `executeRenderWork_` → uploads (`prepareCellUploads`) → `renderAndPresent` from a `SurfaceWindowSnapshot`
   (never the live `Window`) with lane-constructed detached UI draws → fence-value stamping → result fields
   (surface generation, font ack). Generational obsolescence still retires stale packets before any backend work.
4. Teardown: stop admission → detach callbacks → signal exit/cancel + wake all → `drainQueuedFrames` (settle) →
   platform keeps pumping while the lane quiesces → pass-GPU/surface/Renderer release on the lane in reverse
   order → windows/platform die on main → lane joins before channels/contexts die → first error wins.

## Key Files

- `App.Frame.Packet.cppm` — `engine.app:frame.packet`: `FramePacket`, `hasRenderTarget`, `makeFramePacket`.
- `App.Frame.Packet.cpp` — non-inline definitions for the above.
- `App.Frame.RenderLane.cppm/.cpp` — `engine.app:frame.render_lane`: `RenderLane` + request/result/control types.
