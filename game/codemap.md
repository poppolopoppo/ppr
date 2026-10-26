# game/

## Responsibility

`app.game` is the reference client for PPR: an interactive generated colony plus headless smoke checks. It owns game state and policy, not engine infrastructure. `demo::TurboLarbin : ApplicationEditor` owns the colony driver, simulation packet, tool/selection/follow state, game input mapping, and display drafts/persistence. The editor base owns window, player, camera, viewport, UI service and both render passes.

## Entry and configuration

- `main.cpp`: thin entry point calling `runGame` exported by `game.application`.
- `Game.Application.cppm/.cpp` (`game.application`): resolves invocation through `Game.Config.*`, loads the display overlay through `Game.Display.*`, applies explicitly supplied startup validation settings before GPU work, and dispatches the selected command. Help is generated from schema declarations; invalid usage exits 2. Smoke returns its error value. Interactive `app.run()` errors are printed and returned; unexpected exceptions escaping that boundary invoke `hal::fatalExit`.
- `Game.Config.cppm/.cpp` (`game.config`): typed game command/profile/exit schema, combined CLI and configuration resolution. Distinct command selectors conflict; they are not gameplay toggles. Use `--help` and declarations for the current option set.
- `Game.Display.cppm/.cpp` (`game.display`): draft validation/provenance, persistable overlay merge/encode/atomic write, overlay load, and injected-time Keep/Revert state. Overlay precedence is base < overlay < environment < CLI. The panel produces intents; application-level generation/ack checks govern native/surface changes. See [DisplayControls](../lib/engine/app/DisplayControls.md).
- `Game.WorldgenWorkshop.cppm/.cpp` (`game.application:worldgen_workshop`): authoring session, immutable worker requests, bounded cancellation, preview/export and owned `AuthoredGameSource` preparation. Standalone Start validates the current owned snapshot through the existing WorldJob; publication commits its source for later New Game. Return restores the retained game while preserving the authoring session. See [Workspace usage](../docs/worldgen-content.md#in-game-worldgen-workshop).

## Interactive flow

1. Initialize the editor and lane-affine game matter runtime, then request a candidate session. Independent main-owner simulation service admits worker generation and publishes the CPU driver after acknowledged simulation-resource preparation; a loading presentation is telemetry, not admission permission. Kenney assets are staged but no committed fixture scene is loaded.
2. The main-owner simulation deadline runs independently of render credits/drawable targets, using explicit elapsed admission and pending fixed slices. Presentation update advances platform/input/camera/UI and captures only committed owned `SimFramePacket` state. Preparation validates tick/world-generation coherence and builds owned `GridStagedFrame`; debug instances become `TriangleStagedFrame`. The UI uses client data and result-fed render diagnostics, not live worker caches.
3. Editor render sends owned frame/pass/UI payloads to the render lane. Grid/triangle pass mutation, upload preparation, encoding and presentation run there. Inline hosts consume the same request structure without transport.
4. `game.application:session_service` owns typed control/result delivery. The base frame-credit receiver settles immediately; game acknowledgments, UI controls and native close wait for exclusive main service through GPU publication and dependent Scene commit. FIFO controls preserve repeated toggles/steps; native close is an unordered safe-point latch. World/job stamps reject stale orders. Dropped or unapplied presentation payloads remain pending for a real later frame.
5. Shutdown stops mutating-control admission, detaches/drains matter on the live lane, proves independent frame/control completion while retaining CPU header owners, closes the finite result receiver, drains detached worker owners, and finally delegates service/window teardown. Unknown completion fail-stops before dependent release.

The resident solver owns current/candidate numerical particles. Normal ticks
return compact status, check the captured body epoch, commit one-way Scene
coupling and step Box2D once; normal frames carry a shared GPU view without a
CPU particle mirror or batch upload. Metadata prepares only at world/inventory
admission. Checkpoint, preview, inspection and edit coverage request exact demand
snapshots on existing workers, with simulation admission frozen while UI/render
continues. Complete v5 payloads and atomic file saving remain unchanged.

Preparation readiness separately matches an actual-present resident stamp for
the acknowledged world/job/generation, publication, terrain revision and joint
tick. CPU preparation, queue acceptance and GPU completion do not release it.
The initial expected publication is held stable while the modal is open.

Controls and panel intents are declared in `Game.Application.cpp` and `ui/ColonyPanel.*`: pause/step/speed/regenerate, inspect/dig/build/demolish, agent selection/follow, grid/edit overlays, help, independently visible Colony/Performance, Display and Menu Quit. Regeneration is confirmed and deferred to an update boundary; Quit and native close share the host's dirty-draft/cancellation/drain path.

When the session has a controlled actor, WASD supplies held fixed-tick commands
ahead of camera input. Opposing directions cancel. Focus loss, UI keyboard capture,
pause and new-game requests clear the command. Camera input rates reset when
gameplay retakes key ownership while preserving Inspector follow; arrows and
mouse remain camera controls. Focus centers the metre-sized actor at inspection
zoom and refreshes the viewport offset on resize. Cancellation restores the old
session with a neutral status. Asynchronous close keeps the active render bundle
while the worker disposes the CPU session, rather than applying an empty catalog.

## UI ownership

See [ui/codemap.md](ui/codemap.md). Game-owned presentation now lives under
`ui/`, with stable existing panel module names. `game.ui.model` captures only
queried cells, scalar reports and cached panel-safe agent summaries before the
synchronous draw. Names/rows are valid for that call only; actions dispatch after
it. `game.ui` owns modal/feedback drawing, while this host owns world/camera
projection, simulation, lifecycle and workshop/display transactions. World-job
progress vocabulary lives in `colony/WorldProgress.cppm` independently of the host.

The merged workspace keeps draft index/qualified asset identity in game UI and
defers selection until old controls emit owning edits. Host mutation dispatch
precedes Start and rechecks current validation. A preview-drain phase settles
shared render commands before a generation phase exclusively pumps WorldJob and
loading presentation. Its per-request callable owns the authoring snapshot;
cancel/failure preserves editing, publication commits the future game source.
Return retains driver/camera/pause, Quit uses the unsaved transition. Gameplay
chrome reserves the left rail in picking; native input capture protects the
summary overlay and background feedback cannot cover opaque windows.

## Colony modules and workloads

Matter lifecycle requests continue across main-service turns on the existing
single RenderLane operation slot. Session mutations retain their FIFO/cursor
while frame/grid acknowledgments and the independent close latch remain
collectable. Slot readiness drives notified continuation eligibility; only GPU
fences and existing worker/tool predicates use timed polling. Workshop return
retains its own resume/finalization continuation. Game settles Matter before
the base renderer teardown proof and lane stop.

See [colony/codemap.md](colony/codemap.md) for the `game.colony.*` modules. Sim numerical state remains renderer-independent; the game driver and `SimFramePacket` bridge the explicit renderer-neutral GPU view to app rendering. Physics Scene and ChunkColliders are driver-owned; Box2D stays inside `engine.physics`.

- `--smoke`: separate headless rendering-enabled Application, generated-cell offscreen readback.
- `--auto-exit` / `--exit-after S` bound graceful runs; the retired `--profile100` option is rejected.
- CSV timing, scripted profiling edits/pan, load/time-trial commands and game-local exception interception are retired. HAL profiler scopes and crash handling remain.

## Build and tests

`CMakeLists.txt` registers the colony modules including `game.colony.matter`, plus `game.config`, `game.display`, `game.application` and its `:world_job`, `:worldgen_workshop`, `:session_service` partitions, and thin `main.cpp`. `app.game` links core/app/math/shader/rhi/sim publicly and physics/matter privately. `Game.SessionService.Tests.cpp` exercises the actual production inbox through a real render-lane wait pump and stale tool delivery; it is registered in `game.tests`. Every explicit executable build stages shaders, textures and meshes when present through the independent `app.game.assets` prerequisite. Asset edits, additions and missing deployment files do not require relinking; files removed from source are pruned from deployment.

The game build registers `game.tests`, the split `game.tests.worldgen_preview`
entry, three independently locked GPU lifecycle entries
(`game.tests.matter_edit_gpu`, `matter_coverage_gpu`, `matter_restore_gpu`), and
the `app.game.worldgen_frontier`/`worldgen_hollows` profile validations unless tests
are explicitly disabled. The CPU entry excludes `game/checkpoint_gpu` and the two
heavy preview replay/crop leaves; the worldgen entry uses `PROCESSORS 4`.
Each GPU entry has its own 180-second ceiling and unchanged explicit validated
D3D12/4103-grid/eight-substep/K2 fixture. `game.tests` stages its shader assets
independently. `Game.Config.Tests.cpp` covers invocation compatibility, source
precedence and help; `Game.Display.Tests.cpp` covers drafts, persistence and
Keep/Revert. Engine test targets never depend on these game modules.

The game-owned world coordinator retains immutable requests, job-local cancellation and isolated candidate drivers. Independent worker admission is explicit; default WorldJob callers retain the original loading-presented gate. Cancel/failure before renderer activation admission preserves the old session; activation admission latches commit intent, and post-activation attachment/presentation failure is terminal while owners remain retained through real retirement. Only detached drivers may enter worker cleanup/capture. Normal startup loads the authored pack/profile/seed and initializes one actor. Independent render controls stage/activate owned terrain separately, and the modal remains through real presentation. No frame result or loading acknowledgment is fabricated. Integrated responsiveness remains unverified: cold command finalization occupies the sole device lane for about40s. Matter now uses retained asynchronous operations; the renderer retains its existing synchronous teardown policy. No timeout, resolution, schedule or backend workaround is adopted.


`--worldgen-preview --worldgen-output=<directory>` exports material/biome/temperature/initial-matter P6 PPM overviews and a bounded JSON report through `colony/WorldGen.Export.cpp`. It is a headless authoring path; see `docs/worldgen-content.md` for resolution, orientation and baseline semantics. Mandatory current-state checkpoint envelope v5 is owned by `game.colony.checkpoint`, leaving raw sim snapshot v4 unchanged. Historical seeds never replace evolved columns; detached restored owners stay frozen until runtime attachment.

`--worldgen-preview --worldgen-gpu-output=<directory>` explicitly enables a rendering-capable,
UI-free frozen diagnostic host after generation. `colony/WorldGen.Capture.cpp`
uses the real GridPass, offscreen render targets and GPU readback for metre-based
start/seam/surface/texture closeups. Blocking upload/completion is restricted to
this host; GPU quiescence is enforced before releasing resources. Explicit RHI
validation settings apply before initialization. See [authoring diagnostics](../docs/worldgen-content.md).

The opt-in `--worldgen-acceptance` command is an interactive production-path
acceptance sequence owned by TurboLarbin: stationary/pan/zoom, a presented seam
edit, preview switches and two acknowledged New Games with resource retirement.
It requires authored sessions and reports incomplete shutdown as failure. See
[worldgen authoring and diagnostics](../docs/worldgen-content.md) for scope and
measurement limits. `--worldgen-report` mirrors render-owner observations through
frame results; neither option reads worker-owned pass state from main.

The world-work modal displays one stage-local progress bar: measured raster/staged chunk counts when available, otherwise an explicitly indeterminate animation driven by the injected application clock. It does not infer overall completion or number preview/cleanup states as generation steps.

## Joint world transition

`Game.SessionService` owns value-only transition phases used by the real host
and focused tests. Replacement snapshots/logically detaches the old driver
before candidate GPU preparation, keeps the old Runtime owner valid through
reversible preparation, and treats renderer activation admission as commit
intent. Its exact activation ACK precedes candidate attachment and the
exactly-once WorldJob/host ownership transfer. Old GPU view selection is disabled
between activation admission and joint publication; ordinary frames remain
nonblocking. Pre-activation cancellation drains both candidates before resuming
the retained identity/pause/readiness facts. Post-activation attachment or
unknown completion errors retain the established terminal/quiescence policy.

Workshop Return resumes/binds the retained driver and restores the camera while
remaining frozen. It emits the restored resident bundle with progress UI and
finishes only when the delivered actual-present stamp matches its reservation,
world/job/generation/fingerprint, publication, terrain and joint tick. Activation
alone cannot clear the modal. Close may abandon visibility after owned operations
settle, then follows normal detach/drain. Normal simulation does not wait for
presentation after this one-time transition.

Workshop entry holds an O(1) strong copy of the final jointly committed particle
view before transferring Driver to the capture job. Retained game frames select
that exact lease through `selectWorkshopRetainedParticles`; otherwise a final
publication can revoke the pass's older cached view and reject the whole frame,
including detached UI. Selection requires the retained game identity and stops
at preview activation admission, preview display, finish or another world
transition. The held copy clears at activation/new-world admission, completion
and terminal cleanup; existing renderer epoch/world checks and fence-held readers
remain authoritative. The current correction is implemented pending focused
execution and unchanged interactive verification.

Rollback's retained resume phase owns Driver service through its exact Runtime
acknowledgement. `sessionTransitionOwnsDriverService` routes the host before
ordinary Driver update, including a latched close; the logically detached durable
owner is never updated through the generic path. The route relinquishes service
immediately when old identity/pause/readiness restoration acknowledges. The
existing checkpoint GPU restore leaf uses an admitted queued/ready resume across
multiple host-style turns to exercise that same production routing predicate.
