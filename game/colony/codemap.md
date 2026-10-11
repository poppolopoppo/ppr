# game/colony/

## Responsibility

Game-owned colony simulation, tools, presentation translation and headless workloads. The default world is seeded and uses `engine.sim` geometry. Physics is coordinated by the game driver, not imported into sim or app.

## Module navigation

| Module | Files | Responsibility |
|---|---|---|
| `game.colony` | `Colony.cppm/.cpp` | RAII grid ownership and generation lifecycle |
| `game.application:world_job` | `WorldJob.cppm/.cpp` | Game-owned asynchronous candidate preparation, cancellation, pointer publication and background retirement |
| `game.colony.worldgen` | `WorldGen.cppm/.cpp` | Game-to-`sim::generate` wrapper |
| `game.colony.content` | `Content.cppm/.cpp` | Owned material catalog, typed fields, biome recipes, geometric features and world profiles with finite phase-checked rules, start/resource preflight and initial-matter references; checked references, ranges, ordering and caps; explicit-root manifest/source loader; canonical full-pack identity and bounded CPU texture validation in Content.Pack.cpp. Runtime generator integration remains pending. |
| `game.colony.metric` | `Metric.cppm/.cpp` | Checked 3:1 fine-cell/metre conversions, globally anchored construction footprints, nominal volume and persisted metric compatibility preflight. Actor, collider and renderer migration follows with generated sessions. |
| `game.colony.driver` | `ColonyDriver.cppm/.cpp` | Explicit-time fixed step, pause/step/speed/regenerate, ECS and driver-local physics |
| `game.colony.agents` | `Agents.cppm/.cpp` | Agent state, planning and movement vocabulary |
| `game.colony.buildings` | `Buildings.cppm/.cpp` | Footprints, progressive build/demolition errands, component registration |
| `game.colony.pathfinding` | `Pathfinding.cppm/.cpp` | Bounded resumable BFS over vacuum cells |
| `game.colony.digtool` | `DigTool.cppm/.cpp` | Rectangular dig edits |
| `game.colony.sim_packet` | `SimFramePacket.cppm/.cpp` | Owned simulation snapshots, bounded initial presentation batches, confirmed-prefix cursor, coherence checks and grid staging |
| `game.colony.translator` | `ColonyTranslator.cppm/.cpp` | Resident-chunk translation/cache revisions, presentation invalidation |
| `game.colony.panel` | `ColonyPanel.cppm/.cpp` | ImGui views/state/intents; no renderer/service resolution |
| `game.colony.smoke` | `ColonySmoke.cppm/.cpp` | Headless readback smoke |

Check each `export module` declaration and `../CMakeLists.txt` for exact module names/registration. `Colony.Elements.h` owns element IDs (vacuum is 0).

## Simulation and presentation contracts

`ColonyDriver` owns Colony, ECS, fixed-step accumulator, physics Scene and ChunkColliders. Default tick is 60 Hz; physics uses fixed 1/60 s. The declared steps are tool, build, pathfind, aiplan, movestep and physics. Simulation consumes explicit elapsed time; wall-clock profiling does not enter simulation state.

Main captures `SimFramePacket` after advancing simulation. `makeSimFramePacket` copies simulation identity/counters/chunks; `isSimPacketCoherent` checks tick/world-generation, and `buildGridStagedFrame` uses owned packet data. The interactive render worker never reads mutable driver/grid/translator state. Edit revision manifests round-trip with lane results; only matching consumed payloads acknowledge pending changes. Failed/dropped frames must not silently lose edits. See [game flow](../codemap.md) and [frame transport](../../lib/engine/app/frame/codemap.md).

Translator's submitted/resident counts are not frustum-visible counts. Cache invalidation and sim dirty state are separate concerns; do not clear simulation dirt merely because a descriptor was submitted. Debug triangle instances for agents, paths, grid and edit feedback are actively staged alongside grid tiles.

`ColonyPanel` consumes views and returns intents/chrome; the main client applies actions. Display draft/persistence policy lives in `../Game.Display.*`. Pathfinding/build bounds and diagnostic row inventories live in their declarations; avoid duplicating stale numeric inventories here.

Display controls and telemetry render through `engine.app:ui.widgets.display`.
The colony panel retains draft synchronization and adapts widget actions to game
intents; game code retains validation, transactions, confirmation timing and persistence.

`ColonySmoke` supplies `runColonySmoke(SmokeOptions)` and verifies deterministic simulation contracts plus offscreen GPU readback. Platform profiler markers remain available for performance investigations.

Candidate initialization accepts explicit generation execution dependencies and a publication epoch. Spawn searches, body preparation and collider chunks check the job context; actor spawning uses the requested seed before publication. Runtime legacy regenerate remains available to explicit fixtures while the interactive client creates a separate new session.

`WorldGen` also exposes a factory-only `PreparedWorld`, owning the immutable
catalog, reproduction `WorldDescriptor` and complete numeric `GeneratedWorld`.
Its factories accept an explicit clock and execution dependencies; the path
overload loads the catalog on its calling worker. `Colony::install` adopts this
owner without rerasterizing, while `grid()` preserves the existing edit boundary.
`ColonyDriver::initPrepared` adopts it into an isolated candidate with metre-scale
shared-contour collision and exactly one controlled actor at its validated start.
The interactive job/publication migration remains in progress. Generated-session
tools resolve catalog terrain IDs: dig/demolition preserve protected material;
construction validates complete globally aligned metre tiles, live actor overlap
and pending reservations before recording progressive work.
