# game/colony/

## Responsibility

Game-owned colony simulation, tools, presentation translation and headless workloads. The default world is seeded and uses `engine.sim` geometry. Physics is coordinated by the game driver, not imported into sim or app.

## Module navigation

| Module | Files | Responsibility |
|---|---|---|
| `game.colony` | `Colony.cppm/.cpp` | RAII grid ownership and generation lifecycle |
| `game.colony.worldgen` | `WorldGen.cppm/.cpp` | Game-to-`sim::generate` wrapper |
| `game.colony.driver` | `ColonyDriver.cppm/.cpp` | Explicit-time fixed step, pause/step/speed/regenerate, ECS and driver-local physics |
| `game.colony.agents` | `Agents.cppm/.cpp` | Agent state, planning and movement vocabulary |
| `game.colony.buildings` | `Buildings.cppm/.cpp` | Footprints, progressive build/demolition errands, component registration |
| `game.colony.pathfinding` | `Pathfinding.cppm/.cpp` | Bounded resumable BFS over vacuum cells |
| `game.colony.digtool` | `DigTool.cppm/.cpp` | Rectangular dig edits |
| `game.colony.sim_packet` | `SimFramePacket.cppm/.cpp` | Owned simulation snapshots for presentation, coherence checks, grid staging |
| `game.colony.translator` | `ColonyTranslator.cppm/.cpp` | Resident-chunk translation/cache revisions, presentation invalidation and timing |
| `game.colony.panel` | `ColonyPanel.cppm/.cpp` | ImGui views/state/intents; no renderer/service resolution |
| `game.colony.smoke` | `ColonySmoke.cppm/.cpp` | Headless readback smoke and sustained load host |
| `game.colony.timetrials` | `TimeTrials.cppm/.cpp` | Deterministic CPU-only trial runner |

Check each `export module` declaration and `../CMakeLists.txt` for exact module names/registration. `Colony.Elements.h` owns element IDs (vacuum is 0). `StageTiming.h` is a diagnostic side channel shared by driver, translator and trials; its counter rows are call counts, not durations.

## Simulation and presentation contracts

`ColonyDriver` owns Colony, ECS, fixed-step accumulator, physics Scene and ChunkColliders. Default tick is 60 Hz; physics uses fixed 1/60 s. The declared steps are tool, build, pathfind, aiplan, movestep and physics. Simulation consumes explicit elapsed time; wall-clock profiling does not enter simulation state.

Main captures `SimFramePacket` after advancing simulation. `makeSimFramePacket` copies simulation identity/counters/chunks; `isSimPacketCoherent` checks tick/world-generation, and `buildGridStagedFrame` uses owned packet data. The interactive render worker never reads mutable driver/grid/translator state. Edit revision manifests round-trip with lane results; only matching consumed payloads acknowledge pending changes. Failed/dropped frames must not silently lose edits. See [game flow](../codemap.md) and [frame transport](../../lib/engine/app/frame/codemap.md).

Translator's submitted/resident counts are not frustum-visible counts. Cache invalidation and sim dirty state are separate concerns; do not clear simulation dirt merely because a descriptor was submitted. Debug triangle instances for agents, paths, grid and edit feedback are actively staged alongside grid tiles.

`ColonyPanel` consumes views and returns intents/chrome; the main client applies actions. Display draft/persistence policy lives in `../Game.Display.*`. Pathfinding/build bounds and diagnostic row inventories live in their declarations; avoid duplicating stale numeric inventories here.

`ColonySmoke` supplies `runColonySmoke(SmokeOptions)` and `runLoad100`; `TimeTrials` supplies `runTimeTrials`. The smoke/load paths use GPU resources; trials exercise the driver without rendering. Their workload checks and CSV formats are defined in the implementations, not by proposed clone/particle designs.
