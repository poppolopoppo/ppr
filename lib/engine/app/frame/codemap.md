# lib/engine/app/frame

## Responsibility

`engine.app:frame.packet` is the owned frame packet: values and owned storage only, built on the
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
- Owner-lane pass updates consume the packet snapshot (`update(dt, packet.m_camera)`), never the live `Camera`, so a
  post-capture producer mutation cannot alter render preparation. `render()` consumes the staged packet single-use
  (fail closed when absent) and submits it (worker) or executes it (inline reference) the same frame.
- `RenderLane` — one fixed worker + `Channel`-free `RawChannel` triple (depths 2/4/8: frames/results/control).
  Forward `RenderFrameRequest`s carry `shared_ptr<const FramePacket>`, `DetachedUiDrawData`, owned `GridStagedFrame`/`TriangleStagedFrame`, and reservation epoch/frame identity with `drop_if_full` (full maps to
  capacity-busy: drop the frame, never fail the app); results carry submit value/error/drop/font-ack/surface
  generation; control carries wake/exit/config (exit additionally latches an atomic, so a full queue never blocks
  exit signalling). Exactly one logical consumer per channel; the signalling path never uses
  `wait_if_full`/`flush`/`close`. The lane multiplexes both inbound queues with `select()` (wakeable), runs a
  synchronous handoff for teardown-time lane operations (pass-GPU release, Renderer shutdown, quiescence probes),
  and settles on exit (discards unsubmitted as dropped results, final sweep). Destructor stops + joins before the
  channels die. Thread rule: the lane runs only injected callbacks; its sole cross-thread signal is the wake hint
  (`PulseEvent` + `postEmptyEvent`).
- Grid results distinguish successful CPU staging (`m_grid_staging_applied`)
  from complete atlas publication (`m_grid_stage_applied`). A staged reset is
  not repeated while bounded uploads defer; edit acknowledgments still require
  complete publication and a successful matching settlement.
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

1. Main: `ApplicationEditor::update()` advances camera and captures `FramePacket`; client/game code builds owned pass staging and UI state. No main-thread mutation of worker-owned pass caches.
2. Main: `render()` consumes the staged packet, detaches UI draw data, and submits an owned `RenderFrameRequest` or executes the same request inline. A transport-full outcome drops presentation; it does not acknowledge unapplied edits.
3. Owner lane: `executeRenderWork_` validates packet/surface, applies staged grid/triangle data, updates passes from the packet camera, prepares uploads, encodes and submits, then presents. Results echo the edit manifest and distinguish CPU staging application from GPU submission/completion. The game acknowledges pending edit revisions only on a matching consumed result.
4. Application construction reservations and renderer GPU credits bound live work. Queued/building work also needs reservation accounting; channel depth alone is not a lifetime guarantee.
5. Teardown stops admission, settles queued work and proves quiescence while main pumps events. Owner-lane pass/surface/renderer cleanup precedes joining and destroying lane storage; native windows/platform remain main-thread work. Unknown GPU completion invokes the application fail-stop boundary before release.

Production frame packets carry functional render data. Profiler scopes and bounded operational diagnostics provide observability without bulk event capture buffers or CSV export.

## Key Files

- `App.Frame.Packet.cppm` — `engine.app:frame.packet`: `FramePacket`, `hasRenderTarget`, `makeFramePacket`.
- `App.Frame.Packet.cpp` — non-inline definitions for the above.
- `App.Frame.RenderLane.cppm/.cpp` — `engine.app:frame.render_lane`: `RenderLane` + request/result/control types.

World candidate commands use the independent control channel with one outstanding
owned command until its result is drained. Header admission, batches of at most
eight owned chunk uploads, readiness, matching activation, and cancellation have
job/generation/content identities. Their results dispatch before frame credits
or dirty acknowledgments and work without an admitted frame. Each world command
yields to queued frame work; shutdown drains accepted commands as canceled.
ApplicationEditor routes these commands to GridPass on the owner lane and ignores
ordinary terrain stages belonging to a superseded active generation.
