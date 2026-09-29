# lib/engine/sim/

## Responsibility

`engine.sim` owns deterministic, renderer-independent simulation state for the chunk world: the
fixed cell grid, the fixed-timestep tick driver plus declared system order, the snapshot
(save/load) contract, and the ECS registry/views substrate. Later phases still to come are
  renderer/HRC integration — only the state those phases build on. Scripting/modding attaches
  outside this module (see Flow/Integration) — `engine.sim` never imports a script engine.
  `:footprint` adds plain ECS body, occupancy and sensor components without importing physics.

## Design

- Partitioned umbrella: `Sim.cppm` re-exports `:chunk_grid`, `:ecs`, `:footprint`, `:tick`, `:snapshot`, and `:worldgen`.
- `:chunk_grid` holds the geometry vocabulary (`kChunkEdge`=128, `kChunksPerEdge`=32,
  `kWorldEdge`=4096, `kChunkCount`=1024, `kCellsPerChunk`=16384), the `GlobalCellPos`/`ChunkPos`/
  `LocalCellPos` strong coordinate types with constexpr converters (`chunkOf`, `localOf`,
  `globalOf`, `chunkIndexOf`), the content-free `Cell` (`m_element` + `m_temperature`), and
  `ChunkGrid`. Grid metadata (dirty flag + `EChunkActivity`) exists for every chunk from
  construction; cell payloads materialize on first write (`setCell`/`markDirty`) and are never
  evicted in phase 1, so residency is monotonic and **dirty ⇒ resident**. A non-resident chunk
  reads back as `Cell{}`. `ChunkPos` arguments are programmer invariants (`PPR_ASSERT`);
  `GlobalCellPos` is validated operationally (`result_out_of_range`, `invalid_argument` for
  non-finite temperature). `setCell` marks its chunk dirty; `markDirty` returns void because
  allocation failure follows the engine allocator contract.
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
  entity that does not hold `T`), as do `get<T>` (`nullptr`) and `mask` (empty). Components are
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
   stable Kahn result (lowest id first), listing declared steps only. `runSystems` refuses
   an unbound declared step before invoking any system and executes bound steps in that order.
- `:snapshot` holds `kSnapshotVersion`, `sim::errc` (+ `error_category()`/`make_error_code()`
  and `std::is_error_code_enum` — same shape as `image::errc`), `RngStream`, `ChunkDelta`
  (dense full-chunk run of exactly `kCellsPerChunk` cells — sub-chunk tracking is a later
  phase), `Snapshot`, and `capture`/`apply`/`save`/`load`. A file-local `validate()` gate is
    shared by save/load/apply/restore: version agreement, chunk index bounds, cell count, duplicate
    chunk indices (fail-closed — never resolved last-wins), finite temperatures, and ECS section validity
    (including exact free-slot allocation order with no live, duplicate, or missing dead slots).
  `apply` validates the whole snapshot before touching the grid; `load` checks the
  version first (→ `version_mismatch`), then short reads (→ `truncated`), then malformed counts,
  out-of-range chunk indices, trailing bytes, duplicate chunk indices and non-finite temperatures
  (→ `invalid_payload`).
  `save`/`load` are little-endian: `u32 version`, `u64 seed`, `u32 delta_count`, per delta
  `u32 chunk` + `u32 cell_count` + cells as `u16 element` + `u32` temperature bit pattern, then
    `u32 stream_count` and per stream `u32 system` + `u64 state`; v3 appends ECS version,
    slot count, per-slot generation/alive, free count + head-to-tail `u32` free indices,
     column count, and per-column id/stride/owner rows/raw bytes, then bodies
     version (`u32`), replay cursor (`u64` fixed ticks), body count (`u32`) and
     sorted rows (`u32` slot, `u32` generation, six LE `f32` bit patterns, `u8`
     awake). ECS already round-trips slot generations and head-to-tail free order.
     Runtime body IDs never serialize. Bodies are captured from registered
     `BodyState` views and checked against ECS rows before registry replacement.
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
  Writes go through `setCell`, so every chunk ends dirty and `capture`-ready. The sim never
  names biomes; the numeric ids match the colony-owned `game/colony/Colony.Elements.h`
  registry (vacuum 0, confirmed by the physics `!= 0u` solid test).

## Flow

Simulation work later phases submit stays outside this module: a driver advances
 `FixedTimestep`, runs the bound steps strictly in `StepRegistry::order()` (or runs none if any
 declared step is unbound), writes cells through `ChunkGrid`,
and `capture` → `save` for a checkpoint (`load` → `apply` restores grid deltas;
`restore(registry, snapshot)` replaces ECS state when matching components are registered).
`capture(grid, registry, seed)` includes ECS state. `capture` is purely
observational — it lists the currently dirty chunks in row-major order and leaves the grid alone;
`apply` MERGES: it writes every cell of every delta (the written chunks therefore end up resident
and dirty) and leaves every other chunk's content and dirty state untouched (`clearAllDirty` is the
reset path), so `capture(apply(g)) == snapshot` holds only for a clean `g` and a duplicate-free
snapshot.

Scripting/modding (future step, outside this module): a script host (leaf `engine.script` or
`game`-owned service) binds gameplay/mod logic as `StepRegistry` systems
(`SystemFn = move_only_function<void(Registry&)>`) and drives them via the existing
`FixedTimestep` owner. Script state never enters `Snapshot`; only C++ `Registry`/`ChunkGrid`/
`RngStream` capture. Scripts run single-threaded on the tick thread, receive time/RNG as
injected parameters, and per-call errors isolate without throwing into the run loop.

## Integration

- Depends on: `engine.core` + `engine.math` (public). Nothing from `engine.app`,
  `engine.image`, `engine.mesh`, `engine.shader`, or `engine.rhi`. `engine.math` still propagates
  as a PUBLIC dep through `INTERNAL_PUBLIC_DEPS` although only the `.cpp` files import it — the
  project-setup tool has no narrower mode.
- Consumed by: `engine.tests.sim` (`sim` suite); later phases attach ECS/physics/scene
  consumers above `engine.app`'s boundary, never below it.
- Build: `Sim.cppm`, `chunk_grid/Sim.ChunkGrid.cppm`, `ecs/Sim.Ecs.cppm`, `footprint/Sim.Footprint.cppm`, `tick/Sim.Tick.cppm`,
  `snapshot/Sim.Snapshot.cppm`, `worldgen/Sim.WorldGen.cppm` in `FILE_SET CXX_MODULES`; the matching `.cpp` files as PRIVATE
  sources; `setup_ppr_project(engine.sim INTERNAL_PUBLIC_DEPS engine.core engine.math)`.

## Key Files

- `Sim.cppm` — umbrella re-export only.
- `chunk_grid/Sim.ChunkGrid.cppm` — geometry, `Cell`, `ChunkGrid` interface.
- `chunk_grid/Sim.ChunkGrid.cpp` — lazy residency, dirty/activity bookkeeping.
- `ecs/Sim.Ecs.cppm` — `Entity`, `ComponentMask`, `Registry`, `View` interface.
- `ecs/Sim.Ecs.cpp` — slot lifecycle, column bookkeeping, `sim.ecs` error category.
- `footprint/Sim.Footprint.cppm` — pointer-free BodyState, BodyDefinition, SolidFootprint and Sensor vocabulary.
- `tick/Sim.Tick.cppm` — `FixedTimestep`, `TickReport`, `StepRegistry` interface.
- `tick/Sim.Tick.cpp` — accumulator/policy math, fail-closed declaration + stable Kahn order.
- `snapshot/Sim.Snapshot.cppm` — `sim::errc`, snapshot vocabulary, capture/apply/save/load.
- `snapshot/Sim.Snapshot.cpp` — error category, shared `validate()`, LE codec.
- `worldgen/Sim.WorldGen.cppm` — `generate(seed, grid)` interface.
- `worldgen/Sim.WorldGen.cpp` — seeded hash-noise regions + vacuum carving.
- `CMakeLists.txt` — `engine.sim` target registration (see Integration).
