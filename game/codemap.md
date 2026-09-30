# game/

## Responsibility

Demo executable (`app.game`): `main()` passes a checked `argc`/`argv` span to
`demo::TurboLarbin : ApplicationEditor`, unless `--smoke` selects the separate
headless `pP::colony::runColonySmoke` host before the editor is constructed.
`--time-trials` selects the deterministic `runTimeTrials` runner and
`--load100` the sustained `runLoad100` load host; `--profile*` flags tune the
live Slice 6 profiling fixture on the interactive path.
The interactive editor presents a seeded, generated 4096×4096-cell colony
through its `GridPass` and a compact ImGui status panel. It still initializes
`TrianglePass`, but submits no Kenney fixture scene. `ApplicationEditor` owns
the window, player, camera, viewport, input context, UI, and both passes;
game code owns the colony driver, render translator, and game input mapping.
The editor module ships unconditionally, independent of `BUILD_TESTING`.

## Design

- `main.cpp` — `TurboLarbin` lifecycle hooks return `std::error_code`.
  `initialize()` first initializes `ApplicationEditor`, then generates seed
  `1234567`, installs `PanCameraController` in orthographic XY mode near world
  center (pixel extent × scale 0.5, eye z = -0.5), and registers owned
  `InputAction`s in the player listener at camera mapping priority without
  shadowing camera WASD/wheel or the higher-priority UI. Space toggles pause;
  N single-steps only while paused; 1/2/3 select tick speed; R selects a fresh
  application-boundary random seed, logs it, regenerates, and resets pass
  presentation state. G toggles dig mode and H the highlight; the tool mapping
  (left-button press plus mouse_2d cursor, dispatched before the camera
  mapping) owns the drag gesture and submits the picked rect through
  `requestDig`. Input errors surface through `update()`.
- `update(dt)` runs the editor, advances the fixed-step driver, stages resident
  chunk tiles via `ColonyTranslator`, computes frustum-visible tile/chunk counts
  using `GridPass::planTiles` and the camera snapshot, then draws the Colony
  panel. One chunk-sized tile per chunk makes the two visible counts equal.
  `shutdown()` detaches the game mapping before destroying callbacks/actions,
  resets translator staging, shuts down the driver, then shuts down the editor
  while retaining the first cleanup error.
- `colony/Colony.cppm/.cpp` and `WorldGen.cppm/.cpp` — game-owned colony
  lifecycle and deterministic `sim::generate` world generation; element IDs
  live in `colony/Colony.Elements.h`.
- `colony/ColonyDriver.cppm/.cpp` — seeded `Colony` plus bounded fixed-timestep
  accumulator, pause/step/speed controls, regeneration, and grid/clock access.
  Default tick is 60 Hz (`ColonyDriverDesc::m_tick_hz`); physics integrates at
  fixed 1/60 s. Each stage records wall-clock `StageTimings` under
  `hal::ProfileScope` `Colony.*` names (observation only, never sim state); a
  spawn-count override (`m_agent_count`, Slice 6 fixture) persists across
  regeneration. It owns the driver-local `physics::Scene` and `physics::ChunkColliders` and
  declares a `physics` step in the fixed-timestep driver; box2d stays behind
  `engine.physics`. No rendering or application dependency and no implicit
  clock.
- `colony/ColonyTranslator.cppm/.cpp` — render-thread-confined bridge from
  resident `engine.sim` chunks to `GridPass` tile and cell-material submissions;
  unseen/invalidated chunks upload, vacuum maps to transparent `0xffff`, and
  reset or submission failure clears pass staging/cache. Its submitted-chunk
  counter is not a frustum-visibility count.
- `colony/ColonyPanel.cppm/.cpp` — read-only ImGui overlay for seed, tick,
  simulation time, pause/speed, resident chunks, and frustum-visible counts.
- `colony/ColonySmoke.cppm/.cpp` — `game.colony.smoke` exposes
  `runColonySmoke(argv)`. Its own headless, rendering-enabled `Application`
  generates a colony, renders one `GridPass` frame into an RGBA8 offscreen
  target, checks a generated temperate rock cell by GPU readback, prints a
  PASS/FAIL summary, and does not create an editor window or UI. Slice 5
  dig smoke prints `SMOKE-DIG dug=... result=PASS` (D6 v3 snapshot +
  capture/restore checks, D7 double-run/restore equality); dig CPU-refresh
  evidence (translator/GridPass uploads) stays distinct from this
  GPU-readback rasterization proof. `game.colony.smoke` also exposes
  `runLoad100` (`--load100`: 100-agent sustained driver plus
  translator/GridPass load, 600 fixed-1/60 frames, driver/translator CSVs).

## Flow

1. `main(argc, argv)` builds the checked span. If `argv[1]` is `--smoke`,
   call `runColonySmoke(args)` and return its error value; `--time-trials`
   runs `runTimeTrials()` and `--load100` runs `runLoad100()`; otherwise
   construct `TurboLarbin("ppr", args)` and return `app.run().value()`
   (other flags, including `--profile*`, retain the editor path).
2. Editor initialization bootstraps window/services/shaders/passes/UI,
   generates the default colony, installs orthographic pan/zoom camera and
   registers colony keys. No fixture import, scene merge, or triangle submission.
3. Each interactive frame updates editor and colony, translates resident
   chunks into `GridPass` submissions, plans visible tile counts, and draws
   the panel before editor-owned rendering/presentation.
4. On exit, remove the colony input mapping before its actions, clear staged
   grid tiles, shut down the driver, and perform editor teardown; preserve the
   first error. The smoke path independently tears down its offscreen target,
   grid pass, driver, and base `Application`.
5. `app.game` POST_BUILD stages `assets/shaders/` and `assets/textures/` beside
   the executable, and `assets/meshes/` if it exists. The generated-colony
   render and smoke paths need the staged shaders, not Kenney meshes.

## Integration

- **Consumers**: interactive `app.game` and `app.game --smoke` run configurations.
- **Depends on**: `engine.core`, `engine.app`, `engine.math`, `engine.shader`,
  `engine.rhi`, and `engine.sim` via `game/CMakeLists.txt`, plus PRIVATE
  `engine.physics` (Slice 4 Oracle grant: driver-owned Scene/ChunkColliders;
  box2d types never leak past `engine.physics`). The editor uses
  `ApplicationEditor`, `GridPass`,
  `PanCameraController`, input actions, and ImGui; smoke uses `Application`,
  `Renderer`, `GridPass`, and RHI readback. Runtime `shaders/` are staged.
- **Provides**: process entry point and the 11 `game.colony*` module APIs
  (`game.colony`, `.worldgen`, `.driver`, `.translator`, `.panel`, `.smoke`,
  `.agents`, `.pathfinding`, `.buildings`, `.digtool`, `.timetrials` — one
  `.cppm` plus matching `.cpp` each per `game/CMakeLists.txt`).
  `main.cpp` directly imports `engine.app/core/math/mesh/sim` and `std`,
  plus the buildings/driver/translator/panel/pathfinding/smoke/timetrials
  modules.

## Key Files

- `main.cpp` — editor-facing generated colony, controls, panel, `--smoke`/`--time-trials`/`--load100` dispatch.
- `colony/Colony.Elements.h`, `colony/Colony.cppm/.cpp`, `colony/WorldGen.cppm/.cpp` — element IDs, grid ownership, seeded generation.
- `colony/ColonyDriver.cppm/.cpp` — deterministic fixed-step simulation host.
- `colony/ColonyTranslator.cppm/.cpp` — GridPass submissions and cache invalidation.
- `colony/ColonyPanel.cppm/.cpp` — read-only ImGui colony status overlay.
- `colony/ColonySmoke.cppm/.cpp` — one-frame headless GPU readback smoke host, plus `--load100`.
- `colony/Agents.cppm/.cpp`, `colony/Buildings.cppm/.cpp`, `colony/Pathfinding.cppm/.cpp` — agents, edit domain, budgeted BFS.
- `colony/DigTool.cppm/.cpp` — immediate rect-clear dig edits.
- `colony/TimeTrials.cppm/.cpp` — deterministic headless `--time-trials` runner.
- `colony/StageTiming.h` — plain-header per-stage wall-clock timings shared by driver/translator/trials.
- `colony/codemap.md` — colony module detail map.
- `CMakeLists.txt` — `app.game` modules/dependencies and shader/texture/conditional mesh staging.
