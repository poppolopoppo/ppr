# lib/engine/physics/

## Responsibility

`engine.physics` is the renderer-independent Box2D integration boundary. It owns
three partitions (re-exported by the `Physics.cppm` umbrella, which holds no API
of its own): `:scene` owns the disposable world, the entity-keyed body/sensor
runtime, and the opaque serial-keyed static-body chain API that terrain
colliders build on; `:chunk_colliders` turns caller-owned row-major element-ID
spans into Box2D chain colliders with indexed, diffed, budget-bounded rebuilds;
`:coupling` publishes sorted sensor and contact events, body states, and
observed awake transitions through caller-owned buffers, and reserves two
explicitly unsupported force/velocity seams. `engine.physics` and `engine.sim`
are siblings and neither imports the other; the sim-side pointer-free
`BodyState`/`BodyDefinition` vocabulary (`Sim.Footprint.cppm`) mirrors the
physics structs only in part: `BodyState` alone is field-identical
(`Physics.Scene.cppm:53-62` vs `Sim.Footprint.cppm:19-26`), while
`physics::BodyDefinition` (`Physics.Scene.cppm:30-51`) carries six extra
pose/velocity fields — `m_x`, `m_y`, `m_angle`, `m_velocity_x`,
`m_velocity_y`, `m_angular_velocity` — that `sim::BodyDefinition`
(`Sim.Footprint.cppm:28-43`) lacks. The game driver merges the two sides
via `mirrorDefinition` (`game/colony/ColonyDriver.cpp:40-66`) without a
dependency between the modules.

## Seams

`Scene::validateParticleImpulses` exposes the same const whole-batch preflight
used by apply, allowing a game owner to validate before acknowledging GPU
publication. The caller must retain exclusive pending-tick ownership through
preflight, GPU acknowledgment, impulse apply and Scene step; validation alone
does not grant a mutation lease.

`Scene::commitParticleCoupling` advances that same validated pending frontier
without applying an impulse or waking a body. The game's resident route checks
the captured body states independently before GPU publication, commits this
one-way frontier, then steps once. Bidirectional apply retains its existing
whole-batch checks and delegates only its bookkeeping tail to this commit.

- `:scene` (`Physics.Scene.cppm` / `Physics.Scene.cpp`) — opaque, RAII-owned
  world with fixed-step configuration, body and sensor creation, sensor/contact
  event draining, sleep controls, and generation-safe handles.
  - `Scene::Desc` defaults to gravity `(0, -9.8)`, `m_fixed_dt` 1/60,
    `m_substeps` 4, `m_enable_sleep` true. `initialize` fail-closes on
    non-finite gravity/timestep, zero/overflowing substeps, or double init;
    `shutdown`/`dispose` are idempotent and also run from the destructor.
    `clear()` destroys every entity body while the chunk chains and the shared
    chain body survive, because `ChunkColliders` owns the chain lifecycle.
  - `BodyHandle` pairs `m_entity` with a per-`Scene`-lifetime `m_serial`
    issued from `m_next_serial` starting at 1 (zero is never issued); every
    lookup requires both to match, so a stale handle to a recycled entity
    fails with `invalid_argument`. `createBodies` takes parallel entity /
    definition / handle spans, validates the whole batch up front (nonzero
    unique entities, finite pose/velocity, positive density, non-negative
    friction/restitution, nonzero category bits), and rolls back every body it
    created when any step fails. Bodies opt in to contact, hit, and sensor
    events at creation (Box2D 3.1.1 requires the non-sensor visitor to opt in
    too); `attachSensor` adds an `isSensor` shape carrying a caller `m_kind`,
    and a disabled definition is a validated no-op.
  - Static terrain enters through the serial-keyed chain API:
    `createChunkChain` / `replaceChunkChain` / `destroyChunkChain` over
    `ChainDefinition` (ordered world-space points, explicit exterior ghost
    points for open chains, single material slot, `m_loop` flag). Open chains
    need at least 2 points, loops at least 4; every segment must clear a
    0.01-unit kissing threshold. `replaceChunkChain` creates before destroying
    and asserts the destroy side cannot fail. All chains hang off one shared
    static `m_chain_body`, so entity zero in contact events identifies the
    Scene-owned chunk-chain body.
  - `step()` advances the world by the configured fixed dt, clears and refills
    the sensor-event buffer (dropping pairs whose owner or visitor is unknown),
    and arms the one-shot contact latch. `drainSensorEvents` fail-closes with
    `no_buffer_space` without consuming when the destination is short.
    `drainContactEvents` drains the last step exactly once
    (`m_contacts_drained`): an empty destination skips the drain and leaves
    contacts available, and a short destination fails without consuming so the
    caller can retry with a bigger buffer. Contacts normalize to ascending
    entity pair (swapping the normal with the entities) and sort by pair, then
    end/begin/hit kind, then contact point, normal, and approach speed.
  - Sleep control is `enableSleep` / `setAwake` / `isAwake`; `state()` returns
    the pose plus linear/angular velocity snapshot.
  - Particle coupling uses plain numeric `BodyBoundarySnapshot` records and
    `ParticleImpulse` batches. `beginParticleCoupling` excludes stepping and
    contour-cohort admission until apply/cancel. Apply validates the complete
    sorted identity/serial/source-pose/finite batch before Box2D impulse calls,
    waking only bodies receiving a nonzero impulse. A successfully published
    tick advances a monotonic frontier bound to one world; failed/cancelled
    admission permits retry, while clear/reconstruction resets that binding.
    The Scene owns physical impulse application; no GPU or matter import exists.
    The ledger records submitted impulses. Static/kinematic/fixed-rotation bodies
    and Box2D's existing velocity guard need not realize that full momentum gain;
    this adapter adds no cap and does not overwrite resulting velocities.
- `:chunk_colliders` (`Physics.ChunkColliders.cppm` /
  `Physics.ChunkColliders.cpp`) — accepts caller-owned row-major element-ID
  spans (nonzero means solid, missing chunks mean empty), dirty-position
  deltas, and active interest; rebuilds bounded row-major slices.
  - Vocabulary: `ColliderChunkPos` (`i32` pair, three-way comparable, so it
    keys `std::map`/`std::set`); `ColliderChunkView` (position, `m_elements`
    span of `edge * edge` occupancy samples, `m_needed` flag marking active-body,
    query, or swept-motion interest). Interest also activates neighbouring contour owners;
    quiescent chunks have no chains, and the caller must retain element spans
    for the duration of each call.
  - Solid convention: a cell is solid exactly when its element id `!= 0u`
    (`solid()` in `Physics.ChunkColliders.cpp`), i.e. id 0 is vacuum /
    non-solid. This is the on-disk confirmation cited by
    `game/colony/Colony.Elements.h` (`kElementVacuum = 0u`); a non-resident
    (absent-view) chunk reads as all-empty the same way `sim::ChunkGrid`
    reads a non-resident chunk as `Cell{}`.
  - Extraction: `cellEdges` uses the shared `engine.math` 16-case binary
    contour table over sample centres, with disconnected saddle cases and solid
    on the left. Dual cells have one row-major existing sample-chunk owner.
    `ColliderGridGeometry` supplies cell size/origin and optional protected-solid
    exterior bounds. `extract` traces those edges into `Polyline`s through a
    sorted endpoint index (outgoing half sorted by `from`, incoming half by
    `to`, ordinal tiebreak preserving legacy scan order), dropping tracing
    from O(E^2) to sorts plus O(degree) per step with bit-identical output.
    Open polylines close over neighbouring chunks: `exteriorEdge` picks the
    sharpest exterior continuation owned by another chunk via `turnRank`,
    falling back to a mirrored reflection ghost. Neighbouring chains thus
    provide ghost vertices at seams and Box2D gets smooth cross-chunk contact
    normals.
  - Indexing (the "index" in the latest rebuild work): `ChunkIndex` is a
    fixed-capacity open-addressing map from chunk position to views-span
    ordinal, built once per `rebuildDirtyChunks` call and threaded through
    `findChunk`/`solid`/`cellEdges`/`exteriorEdge`/validation. `kSlots` is
    2048 (twice `sim::kChunkCount`'s 1024, power of two for index masking),
    held as one 8 KiB stack `i32` array — no heap allocation for the index
    itself — with multiplicative hashing
    (`x * 0x9E3779B1 ^ y * 0x85EBCA6Bu`). Per-cell probes (up to 16,384 cells
    times 5 `solid()` calls) and the duplicate-validation scan drop from
    O(views) to O(1) average. A span beyond capacity sets `m_overflow` and
    every lookup falls back to the legacy linear scan with identical results.
    The table is lookup-only and never iterated for output, so drain order,
    budget, neighbour fanout, and chain tracing are untouched.
  - Dirty fanout and budget: each dirty position must itself be present in the
    views span (else `invalid_argument`); it enqueues the existing 3 by 3
    neighbourhood into the `m_pending`
    ordered set. The drain loop takes the smallest pending chunk first and
    stops after `max_chunks`, reporting the per-call count through
    `processed`. The caller resubmits a dirty position only on a new edit —
    the pending set persists across calls, so an under-budget drain resumes
    where it stopped. Chunks grown purely for ring closure that were never
    dirtied are covered because neighbours fan out onto them, not because the
    caller lists them.
  - Diffing (the "diff" in the latest rebuild work): `m_shapes` shadows
    `m_chains` with the previous drain's per-chunk payloads (`StoredChain`:
    points plus both ghosts plus loop flag), because `Scene` exposes no
    chain-content query. Each drain extracts the desired polylines — a pure
    function of (views, edge) — and compares per positional slot with exact
    float equality (`sameGeometry`: loop flag, both ghosts, point count, every
    point). A fully identical chunk skips all `Scene` calls yet still counts
    as processed and leaves pending. Otherwise the splice runs in three
    phases: create-only (added plus changed slots, increasing line order),
    destroy-only (removed plus replaced slots, increasing index order), then a
    non-allocating noexcept splice that keeps held handles in their slots and
    fills the rest with created handles, committing the payload snapshot only
    on success. Any `Scene` failure destroys just the chains created in phase
    1 (plus erasing a pre-inserted shape node) and returns the error, leaving
    the previous chunk collider — both maps and the world — exactly as
    before. Chains are therefore staged before old handles are removed, and a
    failed build retains the previous chunk collider.
  - Validation: `edge == 0`, `edge > 1024`, or `max_chunks == 0` fail fast;
    out-of-range positions, wrong-sized element spans, and duplicate views
    fail closed before any `Scene` call. Allocation failure is fatal and
    propagates after rolling back newly acquired chains/bodies.
  - `materializeForQuery` explicitly activates one chunk even without
    active-body interest: it forces `m_needed` on a copied view set, stashes
    the pending queue aside, rebuilds all affected owners with budget 9, then merges the stashed
    queue back. `chainCount(pos)` / `pendingCount()` are the observable
    inspectors (chain counts back the smoke-test coverage accounting).
- `:coupling` (`Physics.Coupling.cppm` / `Physics.Coupling.cpp`) —
  "coupling" here means the per-tick publish of physics state into
  caller-owned simulation buffers; it owns no bodies, no chains, and no
  memory beyond one previous-awake snapshot (`m_previous`).
  - `publishUpdates` takes caller-supplied sorted entity handles plus
    caller-owned `states` / `sensors` / `wakes` / `contacts` spans and counts.
    Handles must be strictly ascending by nonzero entity (else
    `invalid_argument`); `handles` and `states` must match in size and `wakes`
    must hold at least `handles` entries (else `no_buffer_space`). It reads
    each body state and awake flag, diffs awake against `m_previous` to emit
    `WakeRecord` transitions, drains and canonically sorts sensor events (by
    sensor entity, visitor entity, kind, then begin/end), and drains contacts
    only when the contact destination is nonempty. Nothing is committed until
    every read and drain succeeds, so a failure leaves `m_previous` and all
    counts untouched. The canonical body/wake order and the drain-once contact
    semantics are what make twin-driver determinism comparisons meaningful.
  - Two reserved seams are declared but not wired: `applyBoundedForce`
    (bounded particle forces and torque) and `sampleBoundaryVelocity`
    (boundary velocity sampling) both return `function_not_supported`.
  - Only `engine.tests.sim` exercises `Coupling` today (the
    `ladder_door_vent_sensor_begin_end_are_published` test); the game driver
    mirrors body states itself and does not use this partition.

## Dependencies

`engine.core` and `engine.math` are public target dependencies
(`INTERNAL_PUBLIC_DEPS` in `CMakeLists.txt`); vcpkg `box2d` 3.1.1
(`box2d::box2d`) is private (`EXTERNAL_SYSTEM_PRIVATE_DEPS`). The boundary
holds by construction: Box2D headers (`box2d/box2d.h`) and every `b2*` id
(`b2WorldId`, `b2BodyId`, `b2ShapeId`, `b2ChainId`) appear only in
`Physics.Scene.cpp`, behind the file-local `Scene::Runtime`. No exported
partition mentions Box2D — `:scene` exposes only engine-core scalars, `std`
spans, `std::expected`, and `std::error_code`; `:chunk_colliders` imports `:scene`, `engine.core`, and `std`, while `:coupling` imports `:scene` + `std` only (`Physics.Coupling.cppm:5-6`). Box2D types must
never leak past this module boundary.

Consumers link PRIVATE, mirroring each other: `engine.tests.sim` links
`engine.physics` PRIVATE for its `sim/physics` test group
(`lib/engine/tests/sim/CMakeLists.txt`, `Sim.Physics.Tests.cpp`), and the game
demo links it PRIVATE under the Slice 4 grant (`game/CMakeLists.txt`:
"driver-owned physics Scene + chunk colliders ... box2d stays behind
engine.physics"). The game owner is `game/colony/ColonyDriver`, which holds a
driver-local `physics::Scene m_scene` plus `physics::ChunkColliders
m_colliders` (`ColonyDriver.cppm`), initializes the world with sleep disabled
and the fixed 1/60-tick 4-substep desc, and never touches `Coupling`.
World-unit convention is identity, not a conversion: one physics world unit
covers one cell (`ColonyDriver.cpp`, `wakeBodiesNearEdit`), so the driver's
`mirrorDefinition` copies `sim::BodyState` pose/velocity verbatim into
`physics::BodyDefinition`, agent chunks derive from body positions by plain
truncation (`static_cast<u32>(pose->m_x) / sim::kChunkEdge` in
`agentNeighbourhood`), collider views carry `sim::kChunkEdge` as the edge with
`sim::ChunkPos` cast field-for-field to `physics::ColliderChunkPos`, and
per-tick collider budgets are `kColliderBudget` 2 (`kSpawnColliderBudget` 64
at spawn). (The eye z = -0.5 seen in `game/main.cpp` is a camera placement,
not a physics conversion — physics is 2-D and has no z.) The driver prepares
views over the agent-neighbourhood seed plus one 3x3 ring-closure per entry,
caches and delta-reuses the element copies across ticks, and drains
`m_collider_dirty` only after a successful prepare; it calls
`Scene::clear()` (bodies go, chains stay) on shutdown before disposing the
scene.

Chunk collider edit cohorts retain old chains while bounded replacements are prepared. Scene update tokens exclude stepping until commit/cancel; an active cohort borrows a Scene that must outlive the collider manager. Destroy/cancel the cohort before resetting that Scene.
