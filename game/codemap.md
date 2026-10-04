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
  N single-steps only while paused; 1/2/3 select tick speed; R only opens the
  regen confirm modal (never regenerates inline) — Confirm re-dispatches the
  intent confirmed and defers the regenerate + `translator.reset` to the top
  of the next `update()` via `m_regen_pending` (`main.cpp:846-859`,
  `:2338-2355`). G toggles dig mode, H the edit highlight, V the chunk grid
  overlay, I selects inspect, B opens the build menu, F selects the next
  colonist, F1 toggles help, F3 toggles the Colony/Performance tab, Esc runs
  the cancel chain (stop follow, gesture, tool, pin, selection, Help)
  (`main.cpp:656-750,1007-1034`); the tool mapping
  (left-button press plus mouse_2d cursor, dispatched before the camera
  mapping) owns the drag gesture and submits the picked rect through
  `requestDig`. Input errors surface through `update()`.
- `update(dt)` runs the editor, advances the fixed-step driver, drains edit
  chunks into presentation via `presentEdits`, stages resident chunk tiles via
  `ColonyTranslator`, then resolves hover (`refreshHover_`), submits 3D debug
  instances (`submitDebugInstances_`: bodies, paths, grid, edit outlines), and
  draws the panel plus the ImGui foreground screen-space feedback overlay
  (`drawPanel_` → `drawFeedbackOverlay_`; `main.cpp:2515-2527,2081-2175`).
  Frustum-visible tile/chunk counts come from `GridPass::planTiles` against the
  camera snapshot inside `drawPanel_` (`:1447-1473` called `:2527`), not a
  separate phase. One chunk-sized tile per chunk makes the two visible counts
  equal.
  `shutdown()` cancels the tool gesture, clears agent selection, resets the
  window-focus handle and the pan controller, detaches the game mapping before
  destroying callbacks/actions, resets translator staging, shuts down the
  driver, then shuts down the editor while retaining the first cleanup error
  (`main.cpp:2546-2580`).
- `colony/Colony.cppm/.cpp` and `WorldGen.cppm/.cpp` — game-owned colony
  lifecycle and deterministic `sim::generate` world generation; element IDs
  live in `colony/Colony.Elements.h`.
- `colony/ColonyDriver.cppm/.cpp` — seeded `Colony` plus bounded fixed-timestep
  accumulator, pause/step/speed controls, regeneration, and grid/clock access.
  Default tick is 60 Hz (`ColonyDriverDesc::m_tick_hz`); physics integrates at
  fixed 1/60 s. Six fixed-timestep steps run in declaration order each tick —
  `tool`, `build`, `pathfind`, `aiplan`, `movestep`, `physics`
  (`ColonyDriver.cpp:22-28,175-210`; `build` before `pathfind`). Each stage records wall-clock `StageTimings` under
  `hal::ProfileScope` `Colony.*` names (observation only, never sim state); a
  spawn-count override (`m_agent_count`, Slice 6 fixture) persists across
  regeneration. It owns the driver-local `physics::Scene` and `physics::ChunkColliders`;
  box2d stays behind `engine.physics`. Agent summaries (`AgentSummary`) and dig stats (`DigStats`,
  `ColonyDriver.cppm:24-28,88-111`) feed the panel. No rendering or application dependency and no implicit clock.
- `colony/ColonyTranslator.cppm/.cpp` — render-thread-confined bridge from
  resident `engine.sim` chunks to `GridPass` tile and cell-material submissions;
  unseen/invalidated chunks upload, vacuum maps to transparent `0xffff`, and
  reset or submission failure clears pass staging/cache. Its submitted-chunk
  counter is not a frustum-visibility count.
- `colony/ColonyPanel.cppm/.cpp` — game-owned ImGui panel: owns `EColonyTool`
  (4: inspect/dig/build_wall/demolish), `EColonyPanelTab`, 18 `EColonyIntent`
  values, plus `ColonyShortcut`/`ColonyNotice`/`ColonyUiState`/`ColonyHover`/
  `ColonyPerfView`/`ColonyPreviewView`/`AgentNeedsRow`/`ColonyUiView` and the
  chrome `ColonyUiFrame` it returns (`ColonyPanel.cppm:33-297`). Single export
  `drawColonyUi(driver, view, shortcuts, state) -> ColonyUiFrame`; seed, tick,
  sim time, pause/speed, resident chunks, frustum-visible counts, selection,
  follow, pin/Inspector, feedback overlay, footprint preview, tool strip, help,
  and the Performance tab ride the view/frame, never globals.
- `colony/ColonySmoke.cppm/.cpp` — `game.colony.smoke` exposes
  `runColonySmoke(argv)`. Its own headless, rendering-enabled `Application`
  generates a colony, renders one `GridPass` frame into an RGBA8 offscreen
  target, checks a generated temperate rock cell by GPU readback, prints a
  PASS/FAIL summary, and does not create an editor window or UI. Slice 5
  dig smoke prints `SMOKE-DIG dug=... result=PASS` (D6 v4 snapshot +
  capture/restore checks, D7 double-run/restore equality); dig CPU-refresh
  evidence (translator/GridPass uploads) stays distinct from this
  GPU-readback rasterization proof. `game.colony.smoke` also exposes
  `runLoad100` (`--load100`: 100-agent sustained driver plus
  translator/GridPass load, 600 fixed-1/60 frames, driver/translator CSVs, scripted 4-waypoint pan period-150 plus pan-invariance upload check
   (see the colony map for waypoints).

## Flow

1. `main(argc, argv)` builds the checked span. If `argv[1]` is `--smoke`,
   call `runColonySmoke(args)` and return its error value; `--time-trials`
   runs `runTimeTrials()` and `--load100` runs `runLoad100()`; otherwise
   construct `TurboLarbin("ppr", args)` and return `app.run().value()`
   (other flags, including `--profile*`, retain the editor path).
2. Editor initialization bootstraps window/services/shaders/passes/UI,
   generates the default colony, installs orthographic pan/zoom camera and
   registers colony keys. No fixture import, scene merge, or triangle submission.
3. Each interactive frame serves a pending confirmed regen first, updates editor and colony, presents edits, translates resident
   chunks into `GridPass` submissions, plans visible tile counts, and draws
   the panel plus feedback overlay before editor-owned rendering/presentation.
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
  `main.cpp` directly imports `engine.app/core/math/mesh/sim/rhi` plus `imgui` and `std`,
  plus the buildings/driver/translator/panel/pathfinding/smoke/timetrials
  modules.

## Key Files

- `main.cpp` — editor-facing generated colony, controls, panel, `--smoke`/`--time-trials`/`--load100` dispatch.
- `colony/Colony.Elements.h`, `colony/Colony.cppm/.cpp`, `colony/WorldGen.cppm/.cpp` — element IDs, grid ownership, seeded generation.
- `colony/ColonyDriver.cppm/.cpp` — deterministic fixed-step simulation host.
- `colony/ColonyTranslator.cppm/.cpp` — GridPass submissions and cache invalidation.
- `colony/ColonyPanel.cppm/.cpp` — game-owned colony panel (tools, tabs, intents, chrome frame).
- `colony/ColonySmoke.cppm/.cpp` — one-frame headless GPU readback smoke host, plus `--load100`.
- `colony/Agents.cppm/.cpp`, `colony/Buildings.cppm/.cpp`, `colony/Pathfinding.cppm/.cpp` — agents, edit domain, budgeted BFS with kMaxSearchNodes 4096 and kExpansionsPerTick 256.
- `colony/DigTool.cppm/.cpp` — immediate rect-clear dig edits.
- `colony/TimeTrials.cppm/.cpp` — deterministic headless `--time-trials` runner.
- `colony/StageTiming.h` — plain-header per-stage wall-clock timings shared by driver/translator/trials; 19 rows incl. 4 counter rows.
- `colony/codemap.md` — colony module detail map.
- `CMakeLists.txt` — `app.game` modules/dependencies and shader/texture/conditional mesh staging.
