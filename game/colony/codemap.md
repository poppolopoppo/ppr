# game/colony/

## Responsibility

Slice 1 view-only colony hookup, wired into `main.cpp`. `Colony` RAII-owns the
simulation grid (`init` generates, `shutdown` releases); `generateWorld` is the
thin game→sim wrapper over `sim::generate`. `Colony.Elements.h` is the canonical
element-id registry (including the vacuum id 0 convention). `ColonyDriver` owns
the single `Colony` plus its fixed-step clock and pause/step/speed/regenerate
controls. `ColonyTranslator` stages resident chunks to `GridPass` each frame and
uploads only unseen/explicitly invalidated chunk materials (vacuum becomes
`0xffff`). `ColonyPanel` is the read-only ImGui status overlay.
`ColonySmoke` is the separate headless `--smoke` entry (`runColonySmoke`).
`game.colony.pathfinding` is bounded budgeted BFS over vacuum cells, resumable
across ticks, with counters for the panel.

## Design

- `Colony.Elements.h` — plain header (`#pragma once`, `<cstdint>` only, no
  module dependency): `kElementVacuum` = 0 through `kElementVent` = 7 plus
  nominal per-archetype temperatures. Vacuum 0 is confirmed on disk by the
  physics solid test (`!= 0u` means solid in
  `Physics.ChunkColliders.cpp`) and by the `Cell{}` default.
- `Colony.cppm` (`game.colony`) — `ColonyDesc{m_seed}` (only degree of
  freedom; grid dims are fixed by `engine.sim`) and `Colony` (non-copyable,
  movable; private data first). `init` regenerates from the new descriptor;
  `shutdown` is idempotent; both return `std::error_code`.
- `WorldGen.cppm` (`game.colony.worldgen`) + `WorldGen.cpp` — `generateWorld`
  forwards to `sim::generate`; infallible in practice but keeps the
  `error_code` operational boundary explicit.
- `ColonyDriver.cppm/.cpp` (`game.colony.driver`) — owns one `Colony` plus the
  fixed-step accumulator; explicit elapsed-time input, 5-slice clamp, deterministic
  tick accounting, boundary-owned RNG seeds only. No app/renderer/physics imports.
- `ColonyTranslator.cppm/.cpp` (`game.colony.translator`) — render-thread-confined
  presentation revisions separate from sim dirty flags; submits every resident
  chunk descriptor per frame, copies cell materials only for unseen/invalidated
  chunks, `reset` clears pass staging/cache on regeneration. Never clears sim dirt.
- `ColonyPanel.cppm/.cpp` (`game.colony.panel`) — read-only
  `drawColonyPanel(driver, counts)`; caller supplies presentation counts.
- `ColonySmoke.cppm/.cpp` (`game.colony.smoke`) — `runColonySmoke(argv)` drives a
  headless rendering-enabled `Application` for one offscreen GridPass frame with
  GPU readback; separate from the interactive `TurboLarbin` editor path.
- `Agents.cppm/.cpp` (`game.colony.agents`) — first agents (GOAP-lite):
  durable `Agent` (key/speed/seed-spawn), `Needs` (o2/rest), `Plan`
  (goal/site/bounded actions/pc/replans) + seed-derived `spawnAgents`
  (splitmix64), `planAgent` (first needy agent, own-PathReq refresh),
  `agentIntent` (waypoint follow, ladder-zone climb, presence work/rest).
  Imports sim + pathfinding + buildings; no physics (driver translates).
- `Pathfinding.cppm/.cpp` (`game.colony.pathfinding`) — bounded deterministic
  BFS over vacuum cells (`isWalkable` = `m_element == kElementVacuum`,
  non-resident reads walkable): durable `PathReq`/`PathComp` (bounded inline
  `kMaxPathPts` = 256 waypoints, snapshot-covered, fixed registration order via
  `registerPathfindingComponents`) plus driver-owned transient `Pathfinder`
  scratch (frontier/visited/parents, never snapshotted). `stepPathfinding`
  charges `kExpansionsPerTick` = 256 expansions per call with cross-tick resume;
  `pathCounts` classifies complete/partial/blocked. Imports sim+core only.
- `Buildings.cppm/.cpp` (`game.colony.buildings`) — game-local edit domain:
  `Footprint` (min/size/element/solid/ladder, validated) + `BuildErrand`
  (progressive row-major placement, `kErrandCellsPerTick` = 64). Central
  `registerColonyComponents` (Req, Comp, Footprint, Errand append-only).
  `advanceErrands` writes cells (temperature preserved) + sim dirt + touched
  chunks + finder reset. Driver exposes `buildWall`/`demolish`/`requestPath`/
  `presentEdits`; `build` step runs before `pathfind` each tick.
- `DigTool.cppm/.cpp` (`game.colony.digtool`) — immediate rect clear:
  `DigRect` (min/size, edges in [1, `kMaxDigEdge` = 32]) + `DigResult`
  (exact cleared count + deduped touched chunks). `digCells` validates,
  collects non-vacuum cells row-major (temperature kept), then applies via
  `setCell` (auto-dirties); vacuum cells are no-ops; `kMaxDigCells` = 1024
  budget. Validation/budget failure changes nothing. Imports sim+core only.
- `CMakeLists.txt` (parent `game/`) — colony `.cppm` files via
  `FILE_SET CXX_MODULES`, `.cpp` files as PRIVATE sources; `engine.sim`
  added to `app.game` `INTERNAL_PUBLIC_DEPS`. POST_BUILD shader/texture/mesh
  staging untouched.

## Flow

`Colony::init(desc)` → `colony::generateWorld(seed, grid)` →
`sim::generate(seed, grid)` fills every cell through `setCell` (all chunks
end up dirty), then carves cavern worms and vent shafts to vacuum.
`Colony::shutdown()` resets to a fresh `ChunkGrid`. Interactive:
`TurboLarbin::update` → `driver.update(dt)` → `translator.submit(grid, gridPass)`
→ `drawColonyPanel`; `R` regenerates with a boundary seed + `translator.reset`.
Pathfinding: `PathReq` rows → `stepPathfinding` advances one request per tick →
emplaces `PathComp` on completion → `pathCounts` for the panel.
Dig: `digCells(grid, rect)` validates → collects non-vacuum cells row-major →
applies vacuum writes (`setCell` auto-dirties) → returns `DigResult`
(`m_dug` + deduped `m_touched`); failures change nothing.

## Integration

- Depends on: `engine.core` + `engine.sim` (public via `app.game`);
  translator/panel/smoke additionally consume `engine.app` (GridPass, `Application`,
  `Renderer`) on the game side only. Never upward: sim never includes colony;
  element ids cross the boundary as opaque `u16` documented against this registry.
- Game links `engine.sim` plus PRIVATE `engine.physics` (Slice 4 Oracle grant:
  driver-owned Scene/ChunkColliders; box2d types never leak past `engine.physics`).
- Consumed by: `game/main.cpp` (`TurboLarbin` + `--smoke` dispatch).

## Key Files

- `Colony.Elements.h` — element-id + temperature registry.
- `Colony.cppm` / `Colony.cpp` — `ColonyDesc`, `Colony`.
- `WorldGen.cppm` / `WorldGen.cpp` — `generateWorld` wrapper.
- `ColonyDriver.cppm` / `ColonyDriver.cpp` — fixed-step driver, pause/step/speed.
- `ColonyTranslator.cppm` / `ColonyTranslator.cpp` — grid→GridPass presentation.
- `ColonyPanel.cppm` / `ColonyPanel.cpp` — read-only status overlay.
- `ColonySmoke.cppm` / `ColonySmoke.cpp` — headless offscreen smoke entry.
  Slice 5 dig smoke aggregates `SMOKE-DIG dug=... result=PASS`: D6 pins
  snapshot v3 with capture-is-not-flush, restore-clear, and matched-command
  next-tick checks; D7 proves same-seed live double-run plus symmetric
  restored-pair equality (entity-keyed, per-tick, live-contact waiver).
  CPU-refresh evidence (D5 translator/GridPass upload deltas) is distinct
  from the offscreen GPU-readback pixel proof; both must pass.
- `Pathfinding.cppm` / `Pathfinding.cpp` — bounded deterministic BFS module.
