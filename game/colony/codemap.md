# game/colony/

## Responsibility

Game-owned colony simulation, tools, presentation translation and headless workloads. The default world is seeded and uses `engine.sim` geometry. Physics is coordinated by the game driver, not imported into sim or app.

## Module navigation

| Module | Files | Responsibility |
|---|---|---|
| `game.colony` | `Colony.cppm/.cpp` | RAII grid ownership and generation lifecycle |
| `game.application:world_job` | `WorldJob.cppm/.cpp` | Game-owned asynchronous candidate preparation, cancellation, pointer publication and background retirement |
| `game.colony.worldgen` | `WorldGen.cppm/.cpp` | Game-to-`sim::generate` wrapper |
| `game.colony.content` | `Content.cppm/.cpp` | Owned material catalog, typed fields, biome recipes, geometric features and world profiles with finite phase-checked rules, start/resource preflight and initial-matter references; checked references, ranges, ordering and caps; explicit-root manifest/source loader; canonical full-pack identity and bounded CPU texture validation in Content.Pack.cpp. Normal interactive startup resolves these inputs on the world coordinator. |
| `game.colony.metric` | `Metric.cppm/.cpp` | Checked 3:1 fine-cell/metre conversions, globally anchored construction footprints, nominal volume and persisted metric compatibility preflight. Actor, collider and renderer migration follows with generated sessions. |
| `game.colony.driver` | `ColonyDriver.cppm/.cpp` | Explicit-time fixed step, pause/step/speed/regenerate, ECS and driver-local physics |
| `game.colony.agents` | `Agents.cppm/.cpp` | Agent state, planning and movement vocabulary |
| `game.colony.buildings` | `Buildings.cppm/.cpp` | Footprints, progressive build/demolition errands, component registration |
| `game.colony.pathfinding` | `Pathfinding.cppm/.cpp` | Bounded resumable BFS over vacuum cells |
| `game.colony.digtool` | `DigTool.cppm/.cpp` | Rectangular dig edits |
| `game.colony.sim_packet` | `SimFramePacket.cppm/.cpp`, `SimFramePacket.Diagnostics.cpp` | Frozen diagnostic maps, owned simulation snapshots, bounded initial presentation batches, confirmed-prefix cursor, coherence checks and grid staging |
| `game.colony.translator` | `ColonyTranslator.cppm/.cpp` | Resident-chunk translation/cache revisions, presentation invalidation |
| `game.colony.panel` | `ColonyPanel.cppm/.cpp` | ImGui views/state/intents; no renderer/service resolution |
| `game.colony.smoke` | `ColonySmoke.cppm/.cpp` | Headless readback smoke |

Prepared simulation packets own numeric terrain visuals, catalog identity and
the fine-cell metric. Terrain uploads preserve sparse catalog IDs and vacuum
zero; explicit legacy fixtures retain their transparent sentinel. The driver
exposes a borrowed immutable `contentCatalog()` view, copied at packet capture.
The render lane never borrows the live catalog. Authored texture periods are
bounded for finite float UV arithmetic before generation.

Check each `export module` declaration and `../CMakeLists.txt` for exact module names/registration. `Colony.Elements.h` owns element IDs (vacuum is 0).

## Simulation and presentation contracts

`ColonyDriver` owns Colony, ECS, fixed-step accumulator, physics Scene and ChunkColliders. Default tick is 60 Hz; physics uses fixed 1/60 s. The declared steps are tool, build, pathfind, aiplan, movestep and physics. Simulation consumes explicit elapsed time; wall-clock profiling does not enter simulation state.

Main captures `SimFramePacket` after advancing simulation. `makeSimFramePacket` copies simulation identity/counters/chunks; `isSimPacketCoherent` checks tick/world-generation, and `buildGridStagedFrame` uses owned packet data. The interactive render worker never reads mutable driver/grid/translator state. Edit revision manifests round-trip with lane results; only matching consumed payloads acknowledge pending changes. Failed/dropped frames must not silently lose edits. See [game flow](../codemap.md) and [frame transport](../../lib/engine/app/frame/codemap.md).

Initial presentation distinguishes successful CPU staging/reset from completed
atlas publication. A matching staged reset is latched once across deferred
retries; the visible prefix and dirty acknowledgments still advance only after
successful publication and presentation.

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
The interactive job captures candidate presentation off-main and publishes only after matching GPU activation. Generated-session
tools resolve catalog terrain IDs: dig/demolition preserve protected material;
construction validates complete globally aligned metre tiles, live actor overlap
and pending reservations before recording progressive work.

Presentation captures the validated profile's wall material as the protected
outside-world sample ID. The driver exposes this numeric value alongside the
borrowed catalog; owned packet/stage transport validates its terrain visual
membership before rendering. No synthetic renderer material identity is needed.

Terrain presentation now carries owned decoded albedo images alongside numeric
visual records. GridPass resolves source texture indices through a per-world
BindlessTextureCache, samples with world-space periods and linearized authored
tints, and retains canceled upload owners through a covering graphics fence.
The normal asynchronous game job uses the candidate publication handshake and
configured authored pack. The explicit many-agent profiling fixture stays legacy.
Inspector terrain names use the active catalog; biome and feature footprint
queries use prepared generation metadata. Initial matter is explicitly baseline,
unsimulated metadata and is hidden after a successful terrain change. The
Generation panel reports descriptor, metric and phase counters and reloads via
the confirmed new-game path. Actor inspection distances are in metres.

WorldJob also grants exclusive frozen-session capture through the same single
coordinator/job bound. The caller pauses/mirrors state and transfers the stable
driver owner; takeCaptured returns it for every recoverable outcome before
worker payload cleanup. Callers must defer all live reads/writes/acks during the
lease and restore prior pause state with neutral input.

Interactive drivers enable `requireEditPublication()`. `ColonyDriver.Edits.cpp` owns per-chunk rollback candidates, suspended tick prefixes and bounded collider preparation; owned simulation packets overlay candidate cells only when colliders are ready. Matching complete render revision manifests settle the candidate before gameplay resumes. Paused tools and explicit single-step preserve held tick accounting. Headless numeric fixtures retain their synchronous edit path.
