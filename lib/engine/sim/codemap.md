# lib/engine/sim/

## Responsibility

`engine.sim` owns deterministic, renderer-independent simulation state for the chunk world: the
fixed cell grid, the fixed-timestep tick driver plus declared system order, the snapshot
(save/load) contract, and the ECS registry/views substrate. The game already integrates this state with rendering
through owned simulation packets; the sim module itself has no renderer or script-engine dependency.
  `:footprint` adds plain ECS body vocabulary (`BodyState`/`BodyDefinition` live; `SolidFootprint`
  (Sim.Footprint.cppm:45-50) and `Sensor` (:52-57) declared but zero external refs) without importing physics.

## Design

- Partitioned umbrella: `Sim.cppm` re-exports `:chunk_grid`, `:ecs`, `:footprint`, `:snapshot`, `:tick`, and `:worldgen` (snapshot before tick per Sim.cppm:5-10).
- `:chunk_grid` holds the geometry vocabulary (`kChunkEdge`=128, `kChunksPerEdge`=32,
  `kWorldEdge`=4096, `kChunkCount`=1024, `kCellsPerChunk`=16384), the `GlobalCellPos`/`ChunkPos`/
  `LocalCellPos` strong coordinate types with constexpr converters (`chunkOf`, `localOf`,
  `globalOf`, `chunkIndexOf`), the content-free `Cell` (`m_element` + `m_temperature`), and
  `ChunkGrid`. Grid metadata (dirty flag + `EChunkActivity` + inclusive dirty
  box) exists for every chunk from construction; cell payloads materialize on first write (`setCell`/`markDirty`/`setCells`) and are never
  evicted in phase 1, so residency is monotonic and **dirty ⇒ resident**. A non-resident chunk
  reads back as `Cell{}`. `ChunkPos` arguments are programmer invariants (`PPR_ASSERT`);
  `GlobalCellPos` is validated operationally (`result_out_of_range`, `invalid_argument` for
  non-finite temperature). `setCell` sets the box on first write in a clean chunk, then grows it
  (`DirtyBounds`, superset semantics; clean chunks have no bounds via `dirtyBounds()`)
  (Sim.ChunkGrid.cpp:32-43); `markDirty` sets the full-chunk
  box; `clearDirty`/`clearAllDirty` write 0,0,0,0 and rely on `m_dirty==false` for 
ullopt`
  (Sim.ChunkGrid.cpp:147-166,198-209); `setCells` is the bulk whole-chunk
  writer (`Array&&` adopt overload: validated move-adoption with no copy; pre-materialization is the caller's duty for concurrent use). `markDirty` returns void because
  allocation failure follows the engine allocator contract. `dirtyChunks()`/
  `activeDirtyChunks()` list dirty chunks row-major (the latter restricted to
  `EChunkActivity::active`); `residentCells()` borrows a resident payload.
- `:ecs` (registry + views + snapshot dump/restore surface) holds
  `Entity` (slot index + generation; `static_assert` standard-layout, trivially copyable,
  `sizeof == 8`; a handle is valid while `m_generation != 0` and orders slot-first),
  `ComponentMask`/`ComponentId`/`kMaxComponents` (=64, one 64-bit word), `ecs::errc`
  (`too_many_components` is the partition's only error code, plus `error_category()`/
  `make_error_code()` and `std::is_error_code_enum`), `Registry`, and `View`. `Registry` keeps
  a slot table (LIFO free list; `create()` recycles with the generation `destroy` bumped it to
  and fresh slots start at generation 1, 0 is never issued) and one dense SoA `Column` per
  registered component (`Array<std::byte> m_values` + `Array<Entity> m_owners` +
  `Array<u32> m_rows` reverse lookup + `m_stride`): `destroy` swap-removes the entity from
  every held column (tail row moves into the hole, the moved entity's reverse lookup follows),
  clears the mask, bumps the generation, and recycles; `remove<T>` does the same for one
  column and returns `false` for expected absence (unregistered type, dead/stale handle, or
  entity that does not hold `T`), as do `get<T>` (
ullptr`) and `mask` (empty). Components are
  raw-copied bytes gated by `static_assert` (trivially copyable and `alignof(T) <=
  alignof(std::max_align_t)`); `registerComponent<T>()` is idempotent and fail-closes with
   `too_many_components` at 64. `clear()` empties slots and rows but retains column registration;
    `dump()` captures slot generations/alive flags, head-to-tail free-slot allocation order, and
    id/stride/owner/byte columns; `restore()` validates layouts and complete, unique dead-slot
    coverage before replacing slots/rows and the LIFO free list. `View<RegistryT, Cs...>` is a zero-allocation range: forward
  `begin`/`end` iterators yielding `std::tuple<Entity, Cs&...>` (const `Cs&` for a const
  registry) in ascending slot index order, an unregistered requested type empties the whole
  view, and "no structural mutation while iterating" is a documented precondition with no
  version enforcement. Single-threaded by contract — no locks, no synchronization.
- `:tick` holds `FixedTimestep` (explicit period/slice budget, `alpha()` clamped to [0,1],
  `advance()` → `Expected<TickReport>`, negative elapsed → `invalid_argument`) with
  `EOverrunPolicy::clamp` (drops the excess whole slices) vs `defer` (keeps them as backlog) and
  an owning `OverrunHook` (`std23::move_only_function`) fired once per overrunning advance. It
  also holds `StepRegistry`: ordered named steps whose `declare(name, after)` accepts forward
  references (pending placeholders occupy their id) and is validated **before** anything is
  committed — empty/duplicate name → `std::errc::invalid_argument`, self-reference or cycle
  (back-edge) → `std::errc::resource_deadlock_would_occur`, leaving `m_steps`/`m_order` untouched. `order()` is a
   stable Kahn result (lowest id first), listing declared steps only. `addSystem` fail-closes with
   `invalid_argument` on unknown id/pending/empty fn/rebind (Sim.Tick.cpp:140-147, Sim.Tick.cppm:115-117).
   `runSystems` refuses an unbound declared step before invoking any system and executes bound steps in that order.
- `:snapshot` holds `kSnapshotVersion` (=4), `sim::errc` (+ `error_category()`/`make_error_code()`
  and `std::is_error_code_enum` — same shape as `image::errc`), `RngStream`, `ChunkDelta`
  (rect-scoped run of `m_width * m_height` cells, row-major over the w x h rect at local
  (`m_x`, `m_y`)), `BodyRecord` (:63-80), `BodiesSection` (:82-90), `Snapshot`, and
  `capture`/`apply`/`save`/`load` plus exported `restore()` (Sim.Snapshot.cppm:121).
  ECS section vocabulary (`EcsSection`/`EcsColumnData`, Sim.Ecs.cppm:117-134) lives in `:ecs`.
  A file-local `validate()` gate is
    shared by save/load/apply/restore: version agreement, chunk index bounds, rect bounds
    (non-empty, inside the chunk) with `m_cells.size() == m_width * m_height`, duplicate
    chunk indices (fail-closed — never resolved last-wins), finite temperatures, and ECS section validity
    (including exact free-slot allocation order with no live, duplicate, or missing dead slots).
  `validateBodies` requires finite state, strictly ascending slots, generation match,
  alive-required, records<=slot_count (Sim.Snapshot.cpp:165-195,242).
  `apply` validates the whole snapshot before touching the grid and writes only the rect cells
  (untouched cells keep content and dirty state); `load` returns `truncated` for 0-3 byte input
  before the version compare (Sim.Snapshot.cpp:453-460), then `version_mismatch`, then short reads
  (→ `truncated`), then malformed counts,
  out-of-range chunk indices, rect overflows, trailing bytes, duplicate chunk indices and non-finite temperatures
  (→ `invalid_payload`).
  `save`/`load` are little-endian: `u32 version`, `u64 seed`, `u32 delta_count`, per delta
  `u32 chunk` + `u32 x` + `u32 y` + `u32 width` + `u32 height` + cells as `u16 element` + `u32` temperature bit pattern, then
    `u32 stream_count` and per stream `u32 system` + `u64 state`; v4 appends ECS version,
    slot count, per-slot generation/alive, free count + head-to-tail `u32` free indices,
     column count, and per-column id/stride/owner rows/raw bytes, then bodies
     version (`u32`), replay cursor (`u64` fixed ticks), body count (`u32`) and
     sorted rows (`u32` slot, `u32` generation, six LE `f32` bit patterns, `u8`
     awake — capture always writes `m_awake=true` (Sim.Snapshot.cpp:303), patched later by driver
     (ColonyDriver.cpp:1388-1398)). ECS already round-trips slot generations and head-to-tail free order.
     Runtime body IDs never serialize. Bodies are captured from the registered
     `BodyState` column of the dumped ECS section (sorted by slot index, not via views)
     and checked against ECS rows before registry replacement.
     The replay cursor needs original creation order/definitions, chains and
     deterministic tick inputs supplied by the caller; values alone do not
     resume Box2D solver caches.
   Reserve capacity is bounded by remaining payload bytes before allocation.

- `:worldgen` holds `generate(seed, grid)`: deterministic offline world generation
  over the fixed 4096x4096 grid into a caller-owned `ChunkGrid`. Pure in `(seed, grid)` —
  self-contained splitmix64 cell hashing plus smoothstep-interpolated lattice noise are the
  only entropy (core `randomNumberGenerator()` is hardware-seeded and `RngStream` is
  snapshot-local, so neither is reused). Paints opaque `u16` element ids + temperatures in
  seeded region masks (border ring, frozen band, pocket disc, reservoir lens, street-grid
  district, vent discs over a temperate default) with unconditional geometric cores so every
  archetype exists for any seed, then carves vacuum tunnels/shafts while sparing the border.
  The 1024-chunk fill fans out on `parallel::foregroundPool` with `WaitGroup`
  (Sim.WorldGen.cpp:399-432), not single-threaded.
  Writes go through `setCells` for the fill and `setCell` for carving, so every chunk ends dirty and `capture`-ready. The sim never
  names biomes; the numeric ids match the colony-owned `game/colony/Colony.Elements.h`
  registry (vacuum 0, confirmed by the physics `!= 0u` solid test).

- `:worldgen.plan` owns validated numeric recipes, compiled fields and isolated
  integer random streams. Game content names and files stay outside sim.
- `:worldgen.layout` owns macro sites and their spatial lookup; required biome
  assignment and start-reservation lookup precede chunk evaluation.
- `:worldgen.geometry` derives bounded feature reservations, worm segments and
  deterministic traversal-room connections. Start and sealed-pocket reservations
  are excluded from room anchors; sealed markers take precedence over room markers.
- `:worldgen.raster` indexes geometry by intersecting chunk and evaluates terrain,
  temperature, host-only deposits, caves and ordered template operations in global
  coordinates. `rasterizeGenerationTerrain` returns an owned candidate grid using
  an injected scheduler/context and drains every accepted task before returning.
  The matter and start partitions resolve bounded disjoint fills with contour-clipped
  volume/mass and validate supported actor-footprint routes and exposed resources.
  The result partition orchestrates the full numeric pipeline with an injected
  clock, final terrain validation and canonical terrain/metadata checksums.
  Runtime publication remains pending;
  the legacy `generate` path above remains the production caller until that work
  is complete.

## Flow

Game-specific simulation systems stay outside this module: a driver advances
 `FixedTimestep`, runs the bound steps strictly in `StepRegistry::order()` (or runs none if any
 declared step is unbound), writes cells through `ChunkGrid`,
and `capture` → `save` for a checkpoint (`load` → `apply` restores grid deltas;
`restore(registry, snapshot)` replaces ECS state when matching components are registered).
`capture(grid, registry, seed)` includes ECS state. `capture` is purely
observational — it lists the currently dirty chunks in row-major order, copying only each
chunk's `dirtyBounds` rect, and leaves the grid alone;
`apply` MERGES: it writes every rect cell of every delta (the written chunks therefore end up resident
and dirty) and leaves every other cell's content and dirty state untouched (`clearAllDirty` is the
reset path), so `capture(apply(g)) == snapshot` holds only for a clean `g` and a duplicate-free
snapshot.

## Integration

- Depends on: `engine.core` + `engine.math` (public). Nothing from `engine.app`,
  `engine.image`, `engine.mesh`, `engine.shader`, or `engine.rhi`. `engine.math` still propagates
  as a PUBLIC dep through `INTERNAL_PUBLIC_DEPS` although only the `.cpp` files import it — the
  project-setup tool has no narrower mode.
- Consumed by: `game` (`app.game`: colony driver, render submissions, chunk colliders)
  and `engine.tests.sim` (`sim` suite). Production sim never imports physics: the
  `sim/physics` test group (`Sim.Physics.Tests.cpp`) drives `physics::Scene` pairs against
  sim state, and both `app.game` and `engine.tests.sim` link PRIVATE `engine.physics`
  for their driver-owned scenes.
- Build: `Sim.cppm`, `chunk_grid/Sim.ChunkGrid.cppm`, `ecs/Sim.Ecs.cppm`, `footprint/Sim.Footprint.cppm`, `tick/Sim.Tick.cppm`,
  `snapshot/Sim.Snapshot.cppm`, `worldgen/Sim.WorldGen.cppm` in `FILE_SET CXX_MODULES`; the matching `.cpp` files as PRIVATE
  sources (`:footprint` has no `.cpp`; CMakeLists.txt:7-18, 5 `.cpp` files have `.cpp`);
  `setup_ppr_project(engine.sim INTERNAL_PUBLIC_DEPS engine.core engine.math)`.

## Key Files

- `Sim.cppm` — umbrella re-export only.
- `chunk_grid/Sim.ChunkGrid.cppm` — geometry, `Cell`, `ChunkGrid` interface.
- `chunk_grid/Sim.ChunkGrid.cpp` — lazy residency, dirty/activity bookkeeping.
- `ecs/Sim.Ecs.cppm` — `Entity`, `ComponentMask`, `Registry`, `View`, `EcsSection`/`EcsColumnData` interface.
- `ecs/Sim.Ecs.cpp` — slot lifecycle, column bookkeeping, `sim.ecs` error category.
- `footprint/Sim.Footprint.cppm` — pointer-free BodyState, BodyDefinition, SolidFootprint and Sensor vocabulary.
- `tick/Sim.Tick.cppm` — `FixedTimestep`, `TickReport`, `StepRegistry` interface.
- `tick/Sim.Tick.cpp` — accumulator/policy math, fail-closed declaration + stable Kahn order.
- `snapshot/Sim.Snapshot.cppm` — `sim::errc`, snapshot vocabulary (`BodyRecord`, `BodiesSection`), capture/apply/save/load plus exported `restore()`.
- `snapshot/Sim.Snapshot.cpp` — error category, shared `validate()`, LE codec.
- `worldgen/Sim.WorldGen.cppm` — `generate(seed, grid)` interface.
- `worldgen/Sim.WorldGen.cpp` — seeded hash-noise regions + vacuum carving.
- `CMakeLists.txt` — `engine.sim` target registration (see Integration).

World generation accepts an explicit chunk scheduler, cancellation context and atomic current-stage/chunk progress. Independent chunks use bounded batches of admitted tasks, and all admitted tasks drain before generation returns or rethrows. Serial execution remains explicit and deterministic. The current numeric recipe is retained until compiled data-driven generation replaces it.

The `:worldgen.noise` partition exports numeric `NoiseFieldDesc` and a move-only compiled `NoiseField`. FastNoiseLite v1.1.1 is privately pinned behind `Sim.WorldGen.Noise.cpp`; sampling is immutable and allocation-free. OpenSimplex2, Perlin, squared-Euclidean cellular distance, fBm, ridged noise and progressive domain warp are supported. Compilation validates finite settings, octave/work limits and signed lattice-coordinate bounds. The legacy generator is not switched to this adapter yet; compiled content recipes remain pending.
