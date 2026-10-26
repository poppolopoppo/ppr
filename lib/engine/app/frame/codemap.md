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
  exists yet) + `m_surface_native` and `m_surface_display` (snapshots so the consume lane can create the surface
  without touching the live `Window`). The render-target verdict is derived from `m_framebuffer_size` alone (`hasRenderTarget`):
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
  `wait_if_full`/`flush`/`close`. The lane multiplexes both inbound queues with `select()` (wakeable), runs an
  owned-operation wait for teardown-time lane operations (pass-GPU release, Renderer shutdown, quiescence probes),
  and settles on exit (discards unsubmitted as dropped results, final sweep). Destructor stops + joins before the
  channels die. Thread rule: the lane runs only injected callbacks; its sole cross-thread signal is the wake hint
  (`PulseEvent` + `postEmptyEvent`).
- Grid results distinguish successful CPU staging (`m_grid_staging_applied`)
  from complete atlas publication (`m_grid_stage_applied`). A staged reset is
  not repeated while bounded uploads defer; edit acknowledgments still require
  complete publication and a successful matching settlement.
- Actual presentation separately returns the active world identity and, when
  resident particles were selected, their world/job/generation plus exact
  publication, terrain revision and joint tick. CPU preparation, submission,
  staging consumption and GPU completion do not produce this resident proof.
  The fixed value travels on the existing result stream and owns no resources.
- Ownership != affinity: content owners keep their objects (editor-owned passes, client-owned ImGui context,
  game-owned snapshots/translator); the lane is only the fixed execution affinity.
- Correlated owned operations share the existing synchronous handoff's single slot, rather than a new queue.
  `tryBeginOperation` accepts a caller-constructed `unique_ptr<RenderLaneOperation>` and moves only that fixed
  handle on admission. Failure leaves the handle/callable unchanged. One callable allocation exists before
  admission; one additional allocated record/control block carries its owner and original result. No small-
  callable move or source destructor runs under the slot lock at admission, worker claim or cancellation.
  The opaque ticket observes that unique record through a weak pointer plus
  a monotonic lane sequence (exhaustion rejects before wrap). Stale tickets can retain control-block storage,
  but never retain the lane. Completed work still occupies the slot until correlated `takeOperation`.
  Wrong/consumed/other-lane tickets fail; expected absence means only the matching operation is pending.
- Queued cancellation claims the closure under the slot mutex, then destroys its captures outside all locks.
  Reentrant take remains pending and cancellation reports `settling` during destruction. Running cancellation
  records intent only. Completion separately carries actual callback invocation, original error/category,
  cancellation intent, and terminal transport status observed at take; none proves GPU completion.
  The slot mutex publishes inputs and completion. Completion is published after all callback/capture access,
  then the CV and main wake are signalled outside locks. Stop rejects new admissions but executes already
  accepted work unless explicit queued cancellation wins; take/cancel remain available after stop/join.
  Queued cancellation destroys captures on its caller; invoked work destroys captures on the lane. Capture
  ownership and destructors must support both threads; neither branch destroys arbitrary captures under locks.
- `operationStatus(ticket)` observes pending/completed/invalid under the existing slot mutex without allocating,
  logging, consuming or releasing anything. Accepted tickets remain observable after stop. Separate
  `operationAvailability()` reports live admission and slot availability; completed-unconsumed work is occupied.
  These observations let a host make a notified continuation eligible without a timer: completed/invalid tickets
  need service, pending tickets wait for the existing main wake. Admission waiters recheck availability after
  the release wake emitted outside locks by nonblocking take, correlated wait consumption and abandoned-wait final settlement.
  Observations are hints for subsequent correlated operations, never reservations or GPU completion proof.
- Blocking lifecycle owners explicitly construct a unique callable, admit it with
  `tryBeginOperation`, then use ticket-only `waitOperation`. Ordinary service retains
  nonblocking take/cancel. The same2ms pump slices and30s deadline apply; correlated
  completion keeps invocation, operation error and terminal transport separate. Callers
  retain the original operation-error-first policy. Wrong/consumed/other-lane tickets
  fail without consuming another record; recursive waits on the same record reject busy.
  A queued timeout destroys/takes its not-run record outside locks. Running/settling
  timeout abandons only that exact record, retaining captures/slot until actual settlement
  auto-releases it. An error without completion never permits borrowed dependency release.
  Existing unknown-completion fail-stop and stop/join budgets remain unchanged. There
  is no callable wait overload, legacy sync mode, second owner, queue or transport.
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

Opt-in grid observation requests carry a boolean into the owner-lane executor;
fixed-size counter/resource values return with frame results and are mirrored
only after the normal epoch check. No callback or borrowed pass pointer crosses
back to main for observations. Disabled requests do not sample the pass.
