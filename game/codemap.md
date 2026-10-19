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

1. Initialize the editor, orthographic pan controller, game input mapping, and debug geometry/materials, then request a candidate session. Admit the world job only after a successful loading-screen presentation. Kenney assets are staged but no committed fixture scene is loaded.
2. On admitted update, the editor advances platform/input/camera/UI. Game update handles deferred intents, advances `ColonyDriver` with explicit elapsed time only outside the generation modal, then captures an owned `SimFramePacket` containing simulation identity, counters and chunk copies. Preparation validates tick/world-generation coherence and builds owned `GridStagedFrame`; debug instances become `TriangleStagedFrame`. The UI uses client data and result-fed render diagnostics, not live worker caches.
3. Editor render sends owned frame/pass/UI payloads to the render lane. Grid/triangle pass mutation, upload preparation, encoding and presentation run there. Inline hosts consume the same request structure without transport.
4. `TurboLarbin::onLaneFrameConsumed_` validates matching packet/epoch/revision acknowledgments before clearing pending edit obligations. Dropped or unapplied payloads remain pending for a later frame. CPU consumption is distinct from GPU completion.
5. Shutdown detaches game input/callbacks, resets game staging, shuts down driver resources, and delegates worker/GPU/window teardown to the editor while preserving the first cleanup error.

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

See [colony/codemap.md](colony/codemap.md) for the twelve `game.colony.*` modules. Driver/simulation code is renderer-independent; `SimFramePacket` and translator bridge to owned presentation data. Physics Scene and ChunkColliders are driver-owned; Box2D stays inside `engine.physics`.

- `--smoke`: separate headless rendering-enabled Application, generated-cell offscreen readback.
- `--profile100`: interactive 100-agent fixture for external profilers; `--exit-after S` bounds the run.
- CSV timing, scripted profiling edits/pan, load/time-trial commands and game-local exception interception are retired. HAL profiler scopes and crash handling remain.

## Build and tests

`CMakeLists.txt` registers twelve colony modules plus `game.config` and `game.display`, their implementations, `game.application` with its `:world_job` partition, and the thin `main.cpp`. `app.game` links core/app/math/shader/rhi/sim publicly and physics privately. Every explicit executable build stages shaders, textures and meshes when present through the independent `app.game.assets` prerequisite. Asset edits, additions and missing deployment files do not require relinking; files removed from source are pruned from deployment.

The game build registers the `game.tests` CTest entry plus the `app.game.worldgen_frontier`/`worldgen_hollows` profile validations unless tests are explicitly disabled. `Game.Config.Tests.cpp` covers invocation compatibility, source precedence and help; `Game.Display.Tests.cpp` covers drafts, persistence and Keep/Revert. Engine test targets never depend on these game modules.

The game-owned world coordinator retains immutable requests, job-local cancellation and isolated candidate drivers. Admission and readiness polling do not wait. Cancel/failure preserves the old paused session and restores its previous pause state once; success replaces the whole session. Old/cancelled sessions are disposed on the coordinator. Ordinary close defers the exit latch and drains through control servicing, including when frame admission is busy. The modal remains until the replacement frame is actually presented. Normal startup loads the configured authored pack/profile/seed on the coordinator; the explicit 100-agent profile retains its legacy fixture. The coordinator captures owned terrain uploads before readiness. Independent render controls admit a metadata header and at most eight chunks per admitted update, then prepare and activate the candidate. Only matching activation publishes the CPU session; admitted activation settles before close. Cancellation and Retry include both CPU cleanup and outstanding GPU controls. The modal remains through the first replacement presentation. End-to-end responsiveness remains an acceptance gate.


`--worldgen-preview --worldgen-output=<directory>` exports material/biome/temperature/initial-matter P6 PPM overviews and a bounded JSON report through `colony/WorldGen.Export.cpp`. It is a headless authoring path; see `docs/worldgen-content.md` for resolution, orientation and baseline semantics. The game checkpoint envelope is owned by `game.colony.checkpoint`, leaving raw sim snapshot v4 unchanged.

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
