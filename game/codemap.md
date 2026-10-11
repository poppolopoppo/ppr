# game/

## Responsibility

`app.game` is the reference client for PPR: an interactive generated colony plus headless smoke checks. It owns game state and policy, not engine infrastructure. `demo::TurboLarbin : ApplicationEditor` owns the colony driver, simulation packet, tool/selection/follow state, game input mapping, and display drafts/persistence. The editor base owns window, player, camera, viewport, UI service and both render passes.

## Entry and configuration

- `main.cpp`: thin entry point calling `runGame` exported by `game.application`.
- `Game.Application.cppm/.cpp` (`game.application`): resolves invocation through `Game.Config.*`, loads the display overlay through `Game.Display.*`, applies explicitly supplied startup validation settings before GPU work, and dispatches the selected command. Help is generated from schema declarations; invalid usage exits 2. Smoke returns its error value. Interactive `app.run()` errors are printed and returned; unexpected exceptions escaping that boundary invoke `hal::fatalExit`.
- `Game.Config.cppm/.cpp` (`game.config`): typed game command/profile/exit schema, combined CLI and configuration resolution. Distinct command selectors conflict; they are not gameplay toggles. Use `--help` and declarations for the current option set.
- `Game.Display.cppm/.cpp` (`game.display`): draft validation/provenance, persistable overlay merge/encode/atomic write, overlay load, and injected-time Keep/Revert state. Overlay precedence is base < overlay < environment < CLI. The panel produces intents; application-level generation/ack checks govern native/surface changes. See [DisplayControls](../lib/engine/app/DisplayControls.md).

## Interactive flow

1. Initialize the editor, orthographic pan controller, game input mapping, and debug geometry/materials, then request a candidate session. Admit the world job only after a successful loading-screen presentation. Kenney assets are staged but no committed fixture scene is loaded.
2. On admitted update, the editor advances platform/input/camera/UI. Game update handles deferred intents, advances `ColonyDriver` with explicit elapsed time only outside the generation modal, then captures an owned `SimFramePacket` containing simulation identity, counters and chunk copies. Preparation validates tick/world-generation coherence and builds owned `GridStagedFrame`; debug instances become `TriangleStagedFrame`. The UI uses client data and result-fed render diagnostics, not live worker caches.
3. Editor render sends owned frame/pass/UI payloads to the render lane. Grid/triangle pass mutation, upload preparation, encoding and presentation run there. Inline hosts consume the same request structure without transport.
4. `TurboLarbin::onLaneFrameConsumed_` validates matching packet/epoch/revision acknowledgments before clearing pending edit obligations. Dropped or unapplied payloads remain pending for a later frame. CPU consumption is distinct from GPU completion.
5. Shutdown detaches game input/callbacks, resets game staging, shuts down driver resources, and delegates worker/GPU/window teardown to the editor while preserving the first cleanup error.

Controls and panel intents are declared in `Game.Application.cpp` and `colony/ColonyPanel.*`: pause/step/speed/regenerate, inspect/dig/build/demolish, agent selection/follow, grid/edit overlays, help, Performance and Display UI. Regeneration is confirmed and deferred to an update boundary.

When the session has a controlled actor, WASD supplies held fixed-tick commands
ahead of camera input. Opposing directions cancel. Focus loss, UI keyboard capture,
pause and new-game requests clear the command. Camera input rates reset when
gameplay retakes key ownership; arrows and mouse remain camera controls.

## Colony modules and workloads

See [colony/codemap.md](colony/codemap.md) for the twelve `game.colony.*` modules. Driver/simulation code is renderer-independent; `SimFramePacket` and translator bridge to owned presentation data. Physics Scene and ChunkColliders are driver-owned; Box2D stays inside `engine.physics`.

- `--smoke`: separate headless rendering-enabled Application, generated-cell offscreen readback.
- `--profile100`: interactive 100-agent fixture for external profilers; `--exit-after S` bounds the run.
- CSV timing, scripted profiling edits/pan, load/time-trial commands and game-local exception interception are retired. HAL profiler scopes and crash handling remain.

## Build and tests

`CMakeLists.txt` registers twelve colony modules plus `game.config` and `game.display`, their implementations, `game.application` with its `:world_job` partition, and the thin `main.cpp`. `app.game` links core/app/math/shader/rhi/sim publicly and physics privately. POST_BUILD stages shaders, textures and meshes when present.

`game.tests` registers `GameConfigUnitTests` unless tests are explicitly disabled. `Game.Config.Tests.cpp` covers invocation compatibility, source precedence and help; `Game.Display.Tests.cpp` covers drafts, persistence and Keep/Revert. Engine test targets never depend on these game modules.

The game-owned world coordinator retains immutable requests, job-local cancellation and isolated candidate drivers. Admission and readiness polling do not wait. Cancel/failure preserves the old paused session and restores its previous pause state once; success replaces the whole session. Old/cancelled sessions are disposed on the coordinator. Ordinary close defers the exit latch and drains through control servicing, including when frame admission is busy. The modal remains until the replacement frame is actually presented. Initial graphics preparation captures at most eight chunks per admitted frame and advances only on the matching successful presentation. Minimized/dropped batches retry; the modal remains through the final batch. The render-lane atlas fence remains pending GPU-stage work, so end-to-end responsiveness is not yet established.
