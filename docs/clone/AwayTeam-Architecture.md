# AwayTeam Clone — Architecture (Implementation Contract)

> **Document scope:** Design/proposal record, not a current implementation inventory or repository-wide instruction. Apply its task-specific requirements only when the active task explicitly adopts this design. Verify implementation claims against source/CMake and the current [documentation guide](../README.md); historical paths, counts and phase status may be stale.

> Scope: commercial-grade clone of Klei's **Away Team** (reference game ID 2168390 —
> that ID identifies Klei's title, not this clone; 2D side-scrolling single-duplicant
> platformer + full gas/liquid/heat tile sim; closed playtest Apr 2026 only) `[ONI-REF]`.
> Crew size of 2–4 for co-op is a `[DECISION]` of this contract, not an announced fact.
> Target: PPR engine at `E:/Code/ppr` as the foundation.
>
> **Evidence legend (unified — used throughout):**
> - `[PPR-FACT]` — verified in PPR source/codemaps listed in §1.
> - `[OBSERVED]` — directly observed in repo file contents (path:line cited).
> - `[INFERRED-PT]` — inferred from playtest/task material; needs confirmation.
> - `[TUNING-UNKNOWN]` — numeric tuning not fixed by this contract; placeholder only.
> - `[DECISION]` — binding decision made by this contract.
> - `[ONI-REF]` — ONI reference behaviour/constant; retune expected for the clone.
> - §1 rows stay `[PPR-FACT]`. §3 Away deltas that are uncertain (bladder,
>   −10…60 °C band, thin-air behaviour, fire spread, mission imperatives ch.1,
>   turbine ratios, lubricant recipe) are marked `[TUNING-UNKNOWN]` / `[INFERRED-PT]`.

---

## 1. PPR Ground Truth (what we build on)

| Area | PPR fact | Source |
|---|---|---|
| World geometry | `ChunkGrid` fixed 4096×4096 cells = 32×32 chunks × 128×128 cells (`kChunkEdge=128`, `kChunksPerEdge=32`, `kWorldEdge=4096`, `kChunkCount=1024`, `kCellsPerChunk=16384`) `[PPR-FACT]` | `lib/engine/sim/codemap.md:16-23`, `lib/engine/sim/chunk_grid/Sim.ChunkGrid.cppm` |
| Cell payload | Content-free `Cell{m_element: u16, m_temperature: float}` only. No mass, no pressure, no phase, no gas/liquid fractions `[PPR-FACT]` | `lib/engine/sim/codemap.md:19-20` |
| Residency/dirt | Lazy payload on first write; never evicted (phase 1); dirty ⇒ resident; non-resident reads as `Cell{}` (vacuum 0); per-chunk dirty flag + `EChunkActivity` + inclusive `DirtyBounds`; `setCell`/`setCells`/`markDirty`/`clearDirty`/`clearAllDirty`, `dirtyChunks()`/`activeDirtyChunks()` row-major `[PPR-FACT]` | `lib/engine/sim/codemap.md:20-33` |
| Tick driver | `FixedTimestep` (explicit period/slice budget, `alpha()`∈[0,1], `EOverrunPolicy::clamp` vs `defer`, `OverrunHook`) + `StepRegistry` (ordered named steps, `declare(name,after)` with forward refs, fail-closed validation, stable-Kahn `order()`, `addSystem`/`runSystems`; unbound declared step blocks all systems) `[PPR-FACT]` | `lib/engine/sim/codemap.md:58-68`, `lib/engine/sim/tick/Sim.Tick.cppm` |
| Snapshot codec | `kSnapshotVersion=4`, in-memory LE codec only (`save`/`load` over byte spans, no file I/O): `u32 ver, u64 seed, u32 delta_count`, per-delta `u32 chunk,x,y,w,h` + cells (`u16 element` + `u32` temp bits), `u32 stream_count` + streams, then v4 ECS (generations/alive, free-order, columns) + bodies (`u32` ver, `u64` replay cursor, rows: slot/gen/6×`f32` bits/`u8` awake, always `m_awake=true` at capture, driver patches later). `capture` = dirty-rect copy (observational, not flush); `apply` = merge (writes rect cells, untouched cells keep content+dirt); full pre-touch `validate()` gate; `load`: 0–3 bytes → `truncated`, then `version_mismatch`, short reads → `truncated`, bad counts/indices/rects/trailing/dupes/non-finite → `invalid_payload` `[PPR-FACT]` | `lib/engine/sim/codemap.md:69-103`, `lib/engine/sim/snapshot/Sim.Snapshot.cppm`, `lib/engine/sim/snapshot/Sim.Snapshot.cpp` |
| ECS | `Entity` (slot+gen, 8 B, trivially copyable, gen 0 never issued), `kMaxComponents=64` (one word), dense SoA `Column` (`m_values` bytes + `m_owners` + `m_rows` + stride), raw-copied trivially-copyable components (`alignof ≤ max_align_t`), `registerComponent` idempotent/fail-closed, `destroy` swap-remove, `View<...>` zero-alloc slot-ordered, **single-threaded, no locks** `[PPR-FACT]` | `lib/engine/sim/codemap.md:34-57`, `lib/engine/sim/ecs/Sim.Ecs.cppm` |
| Worldgen | `generate(seed,grid)`: pure in `(seed,grid)`, splitmix64 + smoothstep lattice noise only (no hardware RNG, no `RngStream`); paints opaque `u16` ids + temps in seeded masks, carves vacuum tunnels; fans out on `parallel::foregroundPool` + `WaitGroup`; writes via `setCells` then `setCell` so all chunks end dirty/capture-ready `[PPR-FACT]` | `lib/engine/sim/codemap.md:105-117`, `lib/engine/sim/worldgen/Sim.WorldGen.cpp` |
| Colony driver | `ColonyDriver` 60 Hz default (`m_tick_hz`), 5-slice clamp, `fixed_dt` 1/60 s; six steps per tick in declaration order: `tool, build, pathfind, aiplan, movestep, physics` (`build` before `pathfind`); driver-owned `physics::Scene` + `ChunkColliders` (PRIVATE `engine.physics` grant, box2d never leaks); `mirrorBodyStates`, `snapshotWithBodies`/`restoreBodies`/`rebuildReplayColliders`; `presentEdits` → translator; collider views = cached derived state (never snapshotted); dig queue 256-entry FIFO, wake radius 8 cells `[PPR-FACT]` | `game/colony/codemap.md:42-58`, `game/colony/ColonyDriver.cppm:1-281`, `game/colony/ColonyDriver.cpp:22-28,175-210,930-947,1339-1401` |
| Agents | 3 agents default (`kAgentCount=3`, max 128), GOAP-lite (`Agent/Needs{o2,rest}/Plan{goal,site,8 actions,pc,replans}`), `spawnAgents` splitmix64 prefix-stable, `planAgent`/`agentIntent` (waypoint follow, ladder-zone climb, presence work/rest) `[PPR-FACT]` | `game/colony/codemap.md:75-82`, `game/colony/Agents.cppm:12-36,88` |
| Pathfinding | Bounded budgeted BFS over vacuum cells (`m_element==0` walkable, non-resident walkable); durable `PathReq/PathComp` (256 waypoints, snapshot-covered) + transient `Pathfinder` scratch (never snapshotted); `kExpansionsPerTick=256`, `kMaxSearchNodes=4096` `[PPR-FACT]` | `game/colony/codemap.md:83-90`, `game/colony/Pathfinding.cppm:21,24` |
| Edits | `DigTool`: immediate rect-clear ≤32-edge, ≤1024-cell budget, vacuum no-op, temp kept, fail-closed `[PPR-FACT]`; `Buildings`: `Footprint{min,size,element,solid,ladder}` (≤32-edge), `BuildErrand` progressive row-major `kErrandCellsPerTick=64`, temp preserved, central `registerColonyComponents` order PathReq,PathComp,Footprint,BuildErrand append-only `[PPR-FACT]` | `game/colony/codemap.md:92-105`, `game/colony/DigTool.cppm`, `game/colony/Buildings.cppm:16-68` |
| Rendering | `GridPass`: 1 tile = 1 chunk, **flat-tint quads / optional cell materials, no lighting/texturing/GI** (`grid_tiles.slang`: `vertexGridMain`+`fragmentGridMain`, `GridFrame` 64 B + `GridTilePayload` 32 B); pure-CPU `planTiles(snapshot,submissions)` frustum cull (0.5-cell epsilon); CPU staging + upload cache clamp 1024 (= chunk count); `Renderer` content-free; passes consume `CameraSnapshot`, never mutable `Camera` `[PPR-FACT]` | `lib/engine/app/renderer/codemap.md:111-113`, `lib/engine/app/renderer/App.Renderer.GridPass.cppm/.cpp` |
| Camera | Row-major, row-vector, left-handed, +Z-forward, +Y-up, `[0,1]` depth; `mul(float4,matrix)`, view-projection `view*projection`, no backend transpose/Y-flip `[PPR-FACT]` | `AGENTS.md` (camera contract), `lib/engine/app/scene/` |
| Elements | `Colony.Elements.h`: vacuum 0 … vent 7 (rock/ice/caustic/oil/ruins/abyssal/vent) + nominal K per archetype (rock 293, ice 228, caustic 332, oil 284, ruins 296, abyssal 208, vent 402) `[PPR-FACT]` | `game/colony/Colony.Elements.h:18-35` `[OBSERVED]` |
| Gaps (explicit non-goals today) | **NO fluid/thermal sim, no audio/netcode/scripting, no save slots**; `GridPass` has no lighting/compute `[PPR-FACT]` | Task ground truth, consistent with sim/colony codemaps |

### 1.1 Measured gates (binding on all phases) `[DECISION]`

No phase exits on modelled budgets alone. Each phase gates on measured results:

- **Memory budget:** dense-grid + solver + snapshot + GPU-copy ceilings from §4.1 are verified
  by P0 profiling on min-spec HW (P0 profiling is a release gate, not advisory). The old
  "256 MiB" estimate is withdrawn and replaced by the §4.1 measured-budget table.
- **CPU budget:** `--time-trials` S1/S2 CSVs stay green with fluid workloads enabled
  (gas + liquid + thermal active, not empty-world idle). Per-step `StageTimings` are recorded;
  any new tick step that pushes a trial red blocks phase exit.
- **Bandwidth budget (MP phases):** delta-stream bytes/sec measured under a scripted
  edit + flow workload; join-baseline bytes and resync frequency recorded. No MP phase exits
  without a documented bytes/sec ceiling and a passing fault-injection resync test.
- **Soak:** `--load100`-style soak with fluids stable (no growth in resident set, no tick-time
  drift, replay checksum stable same-build).

---

## 2. Target Stack

| Layer | Choice | Rationale |
|---|---|---|
| Engine | PPR (`engine.sim` + `engine.app` + `engine.physics` + `engine.rhi/shader`), C++23 / C++20 modules, existing presets (`msvc-dev/live/rel`, `clang-cl-*`) `[PPR-FACT]` | Reuse ChunkGrid/tick/snapshot/ECS/GridPass wholesale; `CMakeLists.txt` wins on graph disputes |
| Sim | Extend `engine.sim` in place (cell payload, new tick steps, snapshot v5); keep renderer-independence (sim never imports app/rhi/physics) `[DECISION]` | Preserves `lib/engine/sim/codemap.md: Integration` edge (`sim → core,math` only) |
| Physics | Keep driver-owned `physics::Scene` + `ChunkColliders` behind `engine.physics` PRIVATE link (`game`, `engine.tests.sim` only) `[PPR-FACT]` | Box2d types must not leak; colliders stay derived/unsnapshotted |
| Rendering | `GridPass` (tiles) + `TrianglePass` (agents/props/debug) + new overlay pass for gas/heat/liquid views; Slang sources compiled at startup by `engine.shader` `[DECISION]` | Away is 2D tile-driven; overlays are ONI/Away core UX |
| Audio | New leaf module (e.g. `engine.audio`, miniaudio or platform HAL) — out of sim, driven by snapshot-safe event log `[DECISION]` | PPR has no audio today |
| Netcode | New leaf `engine.net` (or `game`-owned service): **host-state-sync ONLY** (§5) `[DECISION]`; transport pluggable (Steam Networking Sockets for the Steam target) | PPR has no netcode today; lockstep is explicitly out of scope |
| Scripting/mods | Leaf host *outside* `engine.sim` binding `StepRegistry::SystemFn`, per `lib/engine/sim/codemap.md:134-139` `[DECISION]` — deferred past MVP (see §8 deferred list) | Sim contract already reserves this shape |
| Persistence | New save-file layer *above* snapshot codec (`game/colony/save/` or `engine.sim:save_file`), using `engine.core/io` (`IoPort`, `MappedFile`) `[DECISION]` | Snapshot is memory-only today |
| Content | Data packs (JSON + schema-validated codegen to `u16` ids + SHC/TC/tables) — elements, buildings, reactions, missions, strings `[DECISION]` | ONI-scale content cannot live in headers |

Camera/coordinates stay on the PPR convention (§1). World stays 4096² cells unless playtesting forces smaller active windows (see §8).

---

## 3. ONI Baseline (mechanics to clone) + Away Deltas

All values in this section are `[ONI-REF]` task ground truth unless marked otherwise:

- **Thermal:** per-element SHC/TC; conduction between adjacent cells + with buildings/bodies; convection in gases/liquids; phase change with **3 K hysteresis / 1.5 K rebound** `[ONI-REF]`.
- **Pressure/flow:** gas/liquid pressure from mass (`zmax`-style compressibility formula), pressure-driven displacement, **vapor displaces O₂** (Away-delta emphasis) `[ONI-REF]`. Conservation plus capped per-tick transfers plus the `zmax`-style pressure note are `[ONI-REF]` to retune — not clone tuning.
- **Power:** per-building watt draw/generation, circuit capacity, **brownout** behaviour when overloaded `[ONI-REF]`; **hand-wheel (manual generator) → hub first** power source in Away (no free starting grid) — Away-delta `[INFERRED-PT]`.
- **Plumbing:** liquid 600 g/s, gas 250 g/s packet flow through pipes `[ONI-REF]`; **non-grid pipes** in Away (pipes as routed graph overlays, not tile-solid grid) — Away-delta `[INFERRED-PT]`.
- **Automation:** 1-bit logic networks `[ONI-REF]`. Ribbon (4-bit bundled) networks are deferred past MVP (see §8) — not part of the MVP contract.
- **Time:** 600 s cycle = 24 × 25 s blocks (schedule UI hangs off this) `[ONI-REF]`. Scheduling UI is deferred past MVP (see §8); the cycle clock itself ships as wall-clock meta in the save footer.
- **Duplicant model (Away deltas):** single-duplicant start, side-scrolling platformer locomotion; **stamina tax** on actions `[INFERRED-PT]`; **O₂ / bladder / temperature tolerance band −10…60 °C** — bladder and the −10…60 °C band are `[TUNING-UNKNOWN]`; **thin-air behaviour** (low-pressure effects) is `[TUNING-UNKNOWN]`; **electrolysis plates** (early O₂ via water-splitting building, not ONI's electrolyzer-then-pipe chain) `[INFERRED-PT]`; **fire spread/damage tuning** is `[TUNING-UNKNOWN]`; **mission imperatives ch.1** (objective chain) replace free-form win — ch.1 content is `[INFERRED-PT]`, MVP ships a placeholder (see §8); **turbine ratios** are `[TUNING-UNKNOWN]`; **lubricant recipe** is `[TUNING-UNKNOWN]`; **cut for Away scope: morale/stress/skills/printing-pod/germs/rockets** — do not implement in MVP `[DECISION]`.

Implication: duplicant needs shrink to `{o2, stamina, bladder, warmth}` and a mission tracker; automation/plumbing/power stay ONI-faithful but re-scoped to one body + co-op crew (§5).

---

## 4. Sim Extensions

### 4.1 Cell payload + snapshot bump `[DECISION]` (extends `[PPR-FACT]` §1 rows)

Single-element displacement: each cell holds exactly one element at a time; displacement
(liquid sinks, gas rises, vapor displaces O₂) moves mass between cells, never blends two
elements inside one cell in the MVP. No mixtures in the MVP cell state `[DECISION]`.

```cpp
// engine.sim:chunk_grid — v5 payload (single-element displacement [DECISION]).
struct Cell {
  uint16_t m_element;  // element id (data-pack codegen, vacuum = 0 preserved).
  uint16_t m_flags;    // solid | liquid | gas | vacuum, insulated, on-fire.
  float    m_temp_K;    // kelvin, finite-validated (existing validate() rule retained).
  float    m_mass_kg;   // kg in cell; pressure derived via element compressibility [ONI-REF].
};
```

- `sizeof(Cell)` budget: 2 + 2 + 4 + 4 = 12 bytes (verify with `static_assert` on target
  compilers; allow padding to 12–16 and budget the padded size). Sidecar/debris-film
  (secondary liquid film, solid debris) is deferred — not in the MVP dense payload.
- Measured budget table (4096 × 4096 = 16,777,216 cells; replaces the withdrawn 256 MiB claim):

| Buffer | Sizing rule | Bound at 12 B/cell | Gate |
|---|---|---|---|
| Dense grid, fully resident | 16,777,216 × `sizeof(Cell)` | ≈ 192 MiB (256 MiB if padded to 16 B) | P0 profiling measures actual resident set with lazy payload; `[DECISION]` gate: dense ships only if P0 passes on min-spec HW |
| Sidecar / debris-film | Deferred past MVP (zero bytes in MVP) | 0 | Revisit only with a new `[DECISION]` + budget row |
| Solver scratch (flow/thermal double-buffer, active chunks only) | Bounded by active-chunk cursor budget (§4.2), never full-grid second copy | Sized by max active chunks × 16,384 cells × scratch/entry | Must be fixed-cap; unbounded growth fails review |
| Snapshot delta stream | Dirty-rect cells only (`capture` shape unchanged) | Per-tick bytes measured under fluid workload (§1.1 bandwidth gate) | MP phases blocked until measured |
| GPU tile upload copy | Chunk-tile staging, clamp 1024 tiles `[PPR-FACT]` | Staging-ring cap, not a full-grid mirror | Overlay invalidation (§4.3) must not force full re-upload |

- Conservation + capped per-tick transfers + `zmax`-style pressure are `[ONI-REF]` to retune:
  flow kernels conserve mass (what leaves one cell enters another, modulo reactions with
  explicit recipes), per-tick transfer caps keep the explicit integration stable, and the
  pressure function shape follows the `zmax`-style reference until P0 retune says otherwise.
- **Snapshot v4 → v5:** bump `kSnapshotVersion`, extend `ChunkDelta` cells with flags/mass,
  extend `validate()` (finite temp + finite non-negative mass + known element ids),
  keep `load` fail-closed ordering (`truncated` → `version_mismatch` → `invalid_payload`).
  v4 migration is explicit: default mass = 0 / vacuum semantics on upgrade-fill, reject with
  `version_mismatch` on unknown element ids — never silently coerce (see §6).
- Element ids remain opaque `u16` across the sim/colony boundary; the data-pack codegen table
  replaces `Colony.Elements.h`'s 8-entry registry (keep the header as the fallback/example pack).

### 4.2 SimActivity (separate from dirty) + canonical tick `[DECISION]`

Presentation-dirty and simulation-activity are different signals and are tracked separately:

- **Presentation dirty (existing `[PPR-FACT]`):** per-chunk dirty flag + inclusive `DirtyBounds`
  feed `capture` deltas and tile re-upload. Overlay/mode changes invalidate the presentation
  cache even when the sim is clean (see §4.3).
- **SimActivity (new `[DECISION]`):** independent per-chunk byte/bitmask recording whether a
  chunk has live simulation work (flow imbalance, thermal gradient, fire, pending building or
  critter work). Rules:
  - Neighbor-wake: a chunk that moves mass/heat across a chunk border wakes the neighbor chunk.
  - Stable traversal: activity iteration order is deterministic (row-major over chunks, same as
    `dirtyChunks()`); activation/deactivation during a sweep never mutates the sweep.
  - Bounded-work cursors with carry-over: each budgeted kernel (flow, thermal) owns a cursor;
    unprocessed active chunks carry over to the next tick instead of blowing the tick budget.
  - `activeDirtyChunks()` alone is insufficient as the activity signal — it conflates
    presentation dirt with sim work and has no wake/carry-over semantics. Kernels iterate the
    activity mask, not the dirty set.
  - Both dirty and activity participate in the save where replay-relevant: grid deltas restore
    content; activity cursors + `RngStreams` + mission state restore determinism (§6).

**Canonical tick (fixed 60 Hz consumption) `[DECISION]`:**

```text
frame-input-capture → tool → build → liquid_flow → gas_flow → thermal → phase →
reactions/fire → power → plumbing → automation → pathfind → aiplan → movestep →
physics → vitals/needs → buildings(lab/farm/cook/electro/turbine) → critters →
hazards(single damage path) → research/mission
```

- `frame-input-capture` seals the intent set for the tick before `tool` runs; late inputs wait
  for the next tick. `tool → build` order is preserved from the driver `[PPR-FACT]`.
- Locomotion steps (`pathfind → aiplan → movestep → physics`) keep their existing relative
  order `[PPR-FACT]` and sit between `automation` and `vitals/needs`.
- Integer divisors (60 Hz base): vitals/hazards/buildings run at 1 Hz (every 60th tick),
  critters at 10 Hz (every 6th tick); all divisors are integers of the 60 Hz consumption
  cadence — no fractional scheduling.
- Power-before-reactions ordering is resolved: `reactions/fire` reads previous-tick settled
  power (committed by the prior tick's `power` step), never same-tick provisional power.
- `hazards` is the single damage path: fire, scald, suffocation, and crush all funnel through
  one damage function so tuning and tests have one target.
- Determinism rules (inherit existing contracts): no wall-clock or hardware RNG inside steps
  (boundary-injected seeds/clocks only), budgeted kernels (`kExpansionsPerTick`-style caps for
  flow/thermal/path so tick cost is bounded), worldgen stays pure `(seed,grid)`.
- All new steps record wall-clock `StageTimings` under `hal::ProfileScope Colony.*`
  (observation only, existing pattern `[PPR-FACT]`), and must respect the `StepRegistry` rule
  that any unbound declared step blocks `runSystems` — declare steps only when their system ships.

### 4.3 Rendering additions `[DECISION]`

Three independent channels — do not conflate them:

- **Tint (packed RGB):** per-cell gameplay tint from the element palette; cheap, always on,
  rides the existing `GridUploadRequest::m_cell_materials` borrowed-span lane `[PPR-FACT]`.
- **Transparency (cut-out cards first):** gas/low-mass cells render as cut-out cards before any
  blended transparency; true alpha blending is deferred until profiled need.
- **Gameplay-lighting (deferred system):** light propagation with occlusion and cross-chunk
  invalidation is a deferred pass, not part of the MVP tile substrate. It invalidates
  presentation tiles independently of sim dirt.
- 8-bucket bootstrap palette requires a palette remap: `grid_tiles.slang:45-52` `[OBSERVED]`
  is not an ordered ramp, so bucket indices must go through a remap table, not a direct cast.
- Overlay mode (heat / O₂ / plumbing / power / automation) is a pass uniform + legend UI.
  Changing the overlay invalidates the presentation cache even when the sim is clean.

---

## 5. Tick / Network Authority for Co-op `[DECISION]`

Away co-op crew of 2–4 is a `[DECISION]` of this contract. PPR has no netcode `[PPR-FACT]`.
Authority model is **host-state-sync ONLY** `[DECISION]` — lockstep is explicitly out of scope:

- **Authority:** host-authoritative fixed-tick simulation. Host owns the `ColonyDriver` tick
  (60 Hz sim, render decoupled via `alpha()`); clients send intents, host commits.
- **Client → host:** timestamped intents (`requestDig`, `buildWall`/`demolish`,
  `requestPath`-equivalents, agent orders, tool drags) — reuse the driver's explicit
  edit-entry API plus its validation so presentation/search invalidation stays coherent.
  Host validates every intent (footprint/dig validation + budget, existing fail-closed rules);
  invalid intents are rejected with a receipt, never applied.
- **Host → clients:** tick id + committed intent receipts + delta stream (reuse `capture`
  dirty-rect deltas + ECS `dump` diffs) + full baseline on join/reconnect. Bodies replicate
  poses/velocities; clients rebuild handles via `restoreBodies`/`rebuildReplayColliders`
  (existing pattern `[PPR-FACT]`); transient state (pathfinder scratch, collider views,
  `StageTimings`, box2d handles) never replicates.
- **Checkpoints:** full baseline capture vs delta ancestry are distinguished (see §6). Deltas
  carry ancestry tick + deletions; checksum covers the full restorable state (grid + ECS +
  bodies + activity cursors + `RngStreams` + mission), not just the deltas. Periodic
  checksum mismatch triggers baseline resync.
- **Ordering/ACK/reconnect/correction:** deltas are ordered per tick; clients ACK; missed ticks
  are re-sent from the host ring buffer while the gap is short, else the host sends a fresh
  full baseline. Corrections arrive as authoritative deltas that overwrite client prediction;
  prediction (if any) never feeds back into host state.
- **Pause policy:** host pauses the tick; clients vote (request-pause) but only the host commits
  pause/resume, broadcast as a tick-flagged state change.
- **Replay-equality** (`D7`-style double-run, `ColonySmoke.cpp` precedent `[PPR-FACT]`) is a
  same-build smoke signal only — not a cross-platform determinism proof. Cross-platform
  equality is not claimed by this contract.
- MVP ships **single-player + local intent log** shaped like net intents so co-op is a
  transport swap, not a sim rewrite (see §8).

---

## 6. Checkpoints and Save File Layer `[DECISION]` (snapshot codec is memory-only `[PPR-FACT]`)

Full baseline capture and delta ancestry are distinct concepts:

- **Baseline capture (full):** complete grid + ECS + bodies + activity cursors + `RngStreams`
  + mission state + data-pack manifest. This is the only payload a fresh join or a load-from-disk
  may start from.
- **Deltas (ancestry):** each delta carries its ancestry tick (the baseline tick it applies to)
  plus deletions (cleared cells, destroyed entities). A delta without a matching ancestry
  baseline is rejected, never partially applied.
- **Transactional apply:** validate → apply-or-reject. The full pre-touch `validate()` gate runs
  before any byte is written; on failure the working state is untouched and the existing
  `truncated` / `version_mismatch` / `invalid_payload` vocabulary is returned.
- **Sidecars** (when they exist post-MVP) participate in dirty, validate, and apply like grid
  data — no silent side-channel state.
- **v4 migration is explicit:** load-v4 → upgrade-fill with default mass = 0 / vacuum semantics;
  reject with `version_mismatch` on unknown element ids. Never silently coerce.
- **No duplicate Registry dump:** ECS content is already in the snapshot; the save envelope must
  not carry a second copy. (The earlier "snapshot bytes + Registry dump" double-count is removed.)

**Envelope:** `PPRSAVE1` magic + `u32` file version + `u64` seed + `u32` snapshot version (5) +
data-pack manifest (ids + hashes) + compressed snapshot payload + thumbnail + mission/options
footer, all LE (match snapshot endianness). CRC32/SHA per section; fail-closed load.
**Slots:** N named slots + autosave ring; atomic write (temp + rename) + fsync; async I/O via
`engine.core/io` (`IoPort`/`MappedFile`) — never block the tick thread. Crash-backup is
deferred past MVP (see §8).
**Content:** baseline-capture bytes + wall-clock meta (cycle/block from 600 s / 24×25 s
`[ONI-REF]`) + player/co-op identities. Collider views, pathfinder scratch, timings, GPU
caches excluded (rebuild on load — current rule `[PPR-FACT]`).
**Compat:** file version gates pack-hash match; v4 payloads migrate-or-reject per §4.1.

---

## 7. Content Schema as Data Packs `[DECISION]`

Replace the 8-entry `Colony.Elements.h` with versioned packs (JSON source, schema-validated, codegen to
`u16` ids + C++ tables). Pack kinds:

- `elements.pack`: id, phase family (solid/liquid/gas), SHC, TC, density/compressibility (`zmax` params `[ONI-REF]`),
  melt/freeze/vaporize/condense points (+hysteresis defaults 3 K/1.5 K `[ONI-REF]`), tint/render params, caloric/fuel values.
- `buildings.pack`: footprint, element, power (W in/out, hand-wheel manual curve first), plumbing ports +
  rates, automation ports (1-bit; ribbon deferred), heat emission, construction cost/time, O₂-adjacent outputs
  (electrolysis plates: water→O₂ recipe lives in `reactions.pack`).
- `reactions.pack`: reactants → products + heat + conditions (fire, electrolysis, phase-adjacent chemistry).
  Turbine ratios and lubricant recipe are `[TUNING-UNKNOWN]` placeholders here until retune.
- `missions.pack`: imperative chain (objectives, triggers, rewards, fail states) — the Away win structure.
  Ch.1 content is `[INFERRED-PT]`; MVP ships the placeholder (§8).
- `biomes.pack`: worldgen region recipes feeding `sim::generate` masks (keeps sim pure `(seed,grid)`:
  packs expand to numeric tables before generation).
- Tooling: schema validator + id allocator (vacuum 0 reserved `[PPR-FACT]`), pack-hash stamping
  (used by net join-check §5 and save header §6).

Cut list enforced by schema: no morale/stress/skills/printing/germs/rockets tables in Away packs
(scope guard `[DECISION]` — validator rejects unknown pack kinds).

---

## 8. Phasing: MVP → Multiplayer

| Phase | Ships | Exit criteria |
|---|---|---|
| P0 spike | Cell sidecar prototype + one thermal step on a test world; heat overlay in `GridPass` materials | 60 Hz holds on target HW (`--time-trials` S1/S2 CSVs green **with fluid workloads**); snapshot v5 round-trips; memory gate (§1.1) measured |
| MVP-1 "Breathe" | Gas flow (O₂/vapor displacement) + electrolysis plates + O₂/bladder/warmth vitals + stamina tax; single-duplicant platformer; hand-wheel→hub power; research T0–T1; farm/cook baseline; mission imperatives ch.1 placeholder; save slots (§6); data-pack v1 (elements/buildings/reactions/missions) | Playable loop: power → O₂ → explore → complete ch.1 placeholder; save/load across sessions |
| MVP-2 "Plumb & Burn" | Liquids, phase change (hysteresis), fire/reactions, non-grid pipes (600/250 g/s `[ONI-REF]`), power circuits + brownout, automation 1-bit, cycle clock (600 s/24×25 s `[ONI-REF]`) | ONI-parity checklist for scoped systems; `--load100`-style soak with fluids stable |
| MP-1 local | Intent-log architecture (§5) in single-player; deterministic replay (same-build smoke, `ColonySmoke.cpp` precedent `[PPR-FACT]`) | Same-seed replay bit-identical same-build; desync detector + baseline-resync path tested via fault injection |
| MP-2 co-op | Host-state-sync net (`engine.net` + Steam sockets), lobby/join, delta sync, 2–4 crew `[DECISION]`; co-op cursors/mission sharing | Closed-playtest-grade stability; bandwidth/CPU budgets documented (§1.1) |

**Deferred past MVP (explicitly out of the MVP contract):** ribbon (4-bit bundled) networks,
scheduling UI, scripting/mods (§2 leaf host), hot-reload (dev presets only when reintroduced),
crash-backup. Each needs its own `[DECISION]` + budget row to re-enter.

Each phase keeps: sim renderer-independent, `StepRegistry` order documented, collider/transient exclusion,
`StageTiming` observation, `Check-SemanticStyle`/`Format-Cpp` gates per repo contract.

---

## 9. Risks / Mitigations

| Risk | Mitigation |
|---|---|
| 16.7M-cell thermal/fluid cost blows 60 Hz | SimActivity iteration (§4.2), per-tick budgets (pathfinding precedent: 256 exp/tick `[PPR-FACT]`), substep/checkerboard, sidecar sparsity; `--time-trials`/`--load100` gates with fluid workloads every phase (§1.1) |
| Cell-memory blowup (§4.1) | 12-byte dense + deferred sidecars + dirty-rect deltas; dense ships only past the P0 measured gate; chunk compression fallback needs a new `[DECISION]` |
| Determinism drift (float order, threading) | Fixed canonical order (§4.2), budgeted integer-friendly kernels, worldgen purity kept; same-build replay-equality smoke as gate (not cross-platform proof) |
| Snapshot/file compat breaks | v5 bump discipline, transactional validate-before-apply (§6), pack-hash gating, explicit migrate-or-reject, atomic slot writes |
| Scope creep (ONI is huge; Away cuts are load-bearing) | Schema-enforced cut list (no morale/stress/skills/printing/germs/rockets `[DECISION]`); mission imperatives bound MVP content; stamina/vitals replace, not add to, cut systems |
| Co-op desync / cheating | Host-state-sync ONLY + intent validation (reuse footprint/dig fail-closed rules `[PPR-FACT]`), full-state checksum + baseline resync, transient-state exclusion |
| Render overlay cost | Tint/transparency/lighting separation (§4.3); palette remap for the 8-bucket bootstrap; no lighting/compute until profiled need |
| Steam/platform risk (transport for the Steam-target ecosystem) | Transport isolated behind `engine.net` interface; single-player intent-log keeps game shippable without MP |

---

## 10. Open Questions (need playtest/product answers)

1. Exact `zmax` pressure formula + per-element compressibility — port ONI values or retune for single-dupe maps? `[ONI-REF]` shape, `[TUNING-UNKNOWN]` numbers.
2. Dense 12 B vs padded 16 B vs sidecar split — decided by P0 profiling gate (§1.1), not by estimate.
3. Snapshot v4 migration default-fill values beyond mass = 0 — product call on unknown-id rejection scope.
4. Host-migration requirement for 2–4 crew sessions? `[DECISION]` needed before MP-2.
5. Save-slot count / Steam Cloud quota / thumbnail size.

---

*Sources re-checked: `codemap.md`, `lib/engine/sim/codemap.md`, `game/codemap.md`,*
*`game/colony/codemap.md`, `game/colony/Colony.Elements.h`, `game/colony/ColonyDriver.cppm`,*
*`game/colony/Agents.cppm`, `game/colony/Buildings.cppm`, `lib/engine/app/renderer/codemap.md`.*
*ONI/Away facts are task-supplied baselines/deltas/refs — flagged inline with the unified legend.*
