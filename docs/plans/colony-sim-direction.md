# PPR Direction: Engine + ONI-like Colony Sim

> Status: DIRECTION — north-star doc, not a task plan. It records where the engine
> and the first game on top of it are heading, why, and in what dependency order.
> Rough feature points from the owner are preserved below, expanded with rationale,
> contracts they must respect, and links to already-designed specs.
> Inspiration: Oxygen Not Included / "Take Away Team"-style Klei colony sims:
> systemic 2D world, base building, networks (power/water/air/heat), agents with
> needs and pathfinding, emergent fire/flood/thermal stories, strong debug tooling.

## 0. Where we stand today

- Slim `Application` owns run loop + platform/services/shader-RHI-`Renderer`
  bootstrap; `ApplicationEditor` owns scene, player, camera, viewport,
  input-context, triangle pass, UI state. `game/main.cpp` hosts `TurboLarbin`,
  a static Kenney cutaway-habitat fixture (~53 placements, fore/mid/back layers).
- Module graph is flat fan-out: `engine.app -> engine.core/math/shader/rhi/image/mesh`.
  `Renderer` is content-free; passes own pipelines, buffers, GPU caches.
  Camera contract: row-major, row-vector, left-handed, +Z-forward, +Y-up, `[0,1]`
  depth; passes consume `SceneView` (CameraSnapshot + RenderView), never a mutable
  `Camera` while drawing.
- Already-designed, not yet (fully) built:
  - `docs/plans/engine.asset-content-pipeline.md` — XNA-style `engine.asset`
    module, `IContentService` vend-only handoff, `.ppak` v1 ABI, cooker, P0–P4.
    This direction doc defers to it for all content-pipeline decisions.
  - `docs/physics/unified_particle_simulation.md` — adaptive unified particle
    matter framework (gas/liquid/granular/solid, split/merge, sleeping, phases
    0–14). This direction doc defers to it for particle-sim decisions.
- This doc adds the missing layers on top: frame pacing, render thread, input
  state machine, ECS, script engine + modding support, asset-type evolutions,
  true shading/GI/TSR/post/GPU-scene, editor GUI, and the colony-sim game itself.

## 1. Engine direction

### 1.1 Better frame pacing with precise OS primitives

**Intent:** stable frame times, measurable jitter, no hidden clocks.

- Use HAL timers (`engine.core:hal` — Windows/Linux/Darwin/Generic) as the single
  precise time source; expose them at the application boundary via an explicit
  injected clock. Lower-level logic never reads an implicit clock (AGENTS.md:
  time is an injected dependency).
- `Application` run loop owns pacing: fixed-timestep simulation accumulator +
  variable render interpolation (or explicit sim/render decoupling once §1.2
  lands), frame-time histogram counters, vsync-aware present pacing.
- Add pacing diagnostics early: per-stage timings (sim, submit, render, present),
  missed-vsync counter, long-frame log with structured fields grouped as one
  block. What gets measured gets fixed.
- Sequencing: first after current work — it is a prerequisite for judging every
  later perf change (render thread, TSR, MPM-PBD).

### 1.2 Dedicated render thread with command channel

**Intent:** game thread simulates and submits; render thread encodes and presents;
no shared mutable renderer state.

- Game thread produces render commands (draw submissions, cache uploads/releases,
  frame constants, `SceneView` snapshots); render thread consumes them through
  the existing lock-free `RawChannel` (MPSC) + `Signal`/`IContext` cancellation
  primitives from `engine.core:concurrency`. See `concurrency-patterns` skill.
- Respects existing contracts:
  - `Renderer` stays content-free (surfaces, queues, submission, presentation).
    Passes keep owning pipelines/buffers/caches.
  - Pass-owned GPU caches are render-thread confined (no internal mutex;
    concurrent upload is a caller bug — already the rule in the asset-pipeline
    plan §2.5).
  - Teardown is inverse of setup: stop submissions → fence/host-wait → release
    CPU scopes → pass shutdown (caches → sampler → pipeline) → renderer
    `waitOnHost` → shader/RHI/platform teardown, retain-first-error, best effort.
- Design questions to answer before coding: single channel vs. per-pass channels;
  synchronous upload handshake (fence) vs. staging-ring ownership transfer;
  backpressure policy when the game thread outruns the render thread;
  how `renderToTexture` (tests, editor thumbnails) works from a non-render thread.
- Sequencing: after §1.1 (need pacing numbers first), before GPU-scene/culling
  work (§3.5) which assumes the threading model is fixed.

### 1.3 InputListener → Unreal-Enhanced-Input-style state machine

**Intent:** chords, priority routing, and mapping switches that behave correctly.

- Today's `InputListener` + filter/analog/device layer + routing latch
  (`lib/engine/app/input/`, 12 files) becomes a persistent-state machine:
  per-action state (idle/ongoing/triggered/cancelled), chord evaluation
  (multi-key/modifier combos as first-class triggers), priority-ordered listener
  stack with correct re-routing when priority or mappings change mid-press
  (no stuck keys, no lost releases).
- Model on Unreal Enhanced Input concepts: actions, triggers, modifiers, mapping
  contexts with priorities — adapted to PPR idioms (`[[nodiscard]]`, `not/and/or`,
  typed IDs, `std::error_code` for operational failures, `PPR_ASSERT` for
  programmer invariants).
- Editor consequence: input-context inspector becomes a first-class toolwindow
  (§4.1); replay/debug of input streams falls out of the state machine's
  explicit states.
- Sequencing: independent of §1.1–§1.2; can run in parallel. Needed before game
  tools (§5.8) which are chord- and context-heavy (dig vs. paint vs. drag
  modifiers, tool priority over camera controls).

### 1.4 Entity Component System for gameplay + rendering/audio glue

**Intent:** gameplay code lives in data + systems, not in renderer/UI singletons;
rendering and audio are sinks that consume ECS state.

- New gameplay substrate (module placement TBD — likely under `engine.app` or a
  new `engine.sim` once the asset module lands; must not invert the dependency
  graph: lower layers never import higher ones).
- Requirements:
  - SoA-friendly component storage, explicit system scheduling, deterministic
    tick order, fixed-timestep simulation matching §1.1 pacing.
  - Rendering glue: systems write submit structs; `Renderer`/passes consume them.
    No scene/content ownership moves into `Renderer` (AGENTS.md rendering contract).
  - Audio glue: same pattern once §2.4 lands (audio components → audio sink).
  - Lifetime rules: visible ownership (RAII/`unique_ptr` own; views/`safe_ptr`
    borrow), injected services, teardown inverse of setup.
- The colony sim (§5) is the first ECS customer: grid cells, buildings, networks,
  agents, particles all become components/systems rather than bespoke managers.
  If the ECS cannot express §5 cleanly, the ECS design is wrong.
- Candidate: EnTT (survey needed, not decided). Survey must cover: C++20-module
  compatibility (no global-header leakage), allocator injection vs. PPR allocator
  hierarchy, threading contract vs. §1.2 render thread, snapshot/serialization
  for save/load (§5.1 world), and whether registry+views cover the submit-struct
  glue or only part of it. Alternative remains a minimal PPR-owned ECS if EnTT
  conflicts with module/lifetime contracts.
- Sequencing: after §1.2 threading model is sketched (systems need to know what
  may run where); survey before any §5 gameplay system.

### 1.5 Config system + module lifecycle manager (undecided — survey needed)

**Intent:** one story for tunables (render scale, sim rates, AI budgets, network
rates, tool defaults) across dev iteration, cooked shipping builds, and
deterministic sim snapshots — without hidden global state — plus one owned place
that brings modules up and down in dependency order.

- Merged direction under evaluation: per-module lifecycle manager + explicit
  config structs. Each module exposes an `initialize(Desc)` / `shutdown()` pair
  (the `IContentService` shape from the asset spec generalizes: typed `Desc` in,
  `error_code` out, shutdown retain-first-error, teardown inverse of setup). A
  small manager owns the ordering (topological over the module graph in AGENTS.md:
  core → math/shader/image/mesh → asset → app), calls init in order and shutdown
  in reverse, and stops new work before releasing (the §1.2 teardown rule writ
  large). Config then has a single entry point: `main` parses CLI + file(s),
  assembles one `Desc` per module, hands the bundle to the manager — so `main`
  stays a thin switchboard and every "config switch" is a visible struct field,
  injectable and snapshot-friendly.
- Against the three options this strengthens (c) and answers its main objection
  (plumbing): the manager *is* the thin bootstrap layer. Open questions it does
  not answer by itself: runtime tuning (editor live-edit needs a scoped mutation
  path, not full re-init), override precedence (defaults < file < CLI < live, or
  explicit rejection of layering), and whether render/editor tunables deserve a
  lighter (a)/(b)-lite lane beside the strict sim `Desc`s.
- Three options on the table, none chosen:
  - (a) Unreal-style cvar + ini: console variables with layered ini files
    (base/project/user/command-line), runtime set/get + editor console. Pro:
    proven iteration speed, uniform override story. Con: global-registry shape
    fights the PPR contracts (no hidden singletons, visible ownership, injected
    dependencies); needs explicit scoping (which cvars are sim-state vs.
    render-local) or determinism/snapshot (§5.0) silently breaks.
  - (b) simdjson + custom registry: JSON configs parsed with the simdjson seam
    already planned for the asset manifest (§2.1 spec §4), typed per-domain
    structs behind a small registry. Pro: reuses the manifest toolchain, no new
    dep, diff-friendly. Con: still a registry — must answer lifetime/override
    precedence and who may mutate at runtime, or it becomes (a) with worse tooling.
  - (c) Fully explicit config structs passed down: each system takes its `Desc`
    at init (like `ContentDesc`), no registry, no lookup. Pro: matches AGENTS.md
    best (functions touch the world only through signatures, teardown inverse of
    setup, trivial snapshots — config is just data). Con: most plumbing; needs a
    thin bootstrap layer (CLI → file → struct assembly) so `main()` doesn't
    become a config switchboard, plus an answer for runtime tuning (editor needs
    *some* live-edit path).
- Survey must answer: sim-vs-render config split (what enters snapshots vs. what
  stays local), override precedence (defaults < file < CLI < editor-live, or
  explicit rejection of layering), runtime mutability rules (who may change what
  mid-tick), shipping story (cooked into packs vs. loose files, fail-closed on
  missing/corrupt), and module fit (C++20-module clean, no global-header leak).
  Likely outcome is a hybrid — (c) for sim-deterministic config, (a)/(b)-lite for
  render/editor tunables — but record the split explicitly instead of drifting.
- Sequencing: decide before ECS systems (§1.4) and sim foundation (§5.0) harden,
  since both take config at init; revisit when hot reload (§2.5) lands (config
  reload rides the same watcher story or is explicitly excluded).

### 1.6 Script engine + modding support

**Intent:** internal gameplay scripting (tools, AI goals, tuning iteration) plus
a future modding story — without breaking determinism or snapshots.

- Decision (recorded, survey closed): primary AngelScript 2.38.0 — refcounted
  with no GC pauses, C++-native binding, console-portable bytecode, actively
  maintained. Fallback is Lua 5.4/5.5 PUC-Rio (no LuaJIT, no sol3). Wren and
  daslang evaluated and deferred on ecosystem grounds. Umka 1.5.x (Go-inspired,
  static types, C-compatible structs, refcount+weak, fibers, C99 + simple C API
  via `umkaAddFunc`/`umkaAddModule`, ~2k stars, Tophat framework) is a
  technically clean leaf fit and better than Lua on determinism, but deferred:
  single-maintainer, small modding pool/tooling vs. AngelScript/Lua, and C-flat
  (not C++-native RAII) interop needs more glue. Revisit only if Go-syntax /
  C-layout preference outweighs reach — then trial as game-only host first.
- Placement: new leaf `engine.script`, PRIVATE on `core` (plus `math` only if
  bindings need it), never PUBLIC. `sim` and `physics` never import it; `sim`
  stays script-free. `game` hosts the `ScriptService` (`IService` plus a
  `StepRegistry::addSystem` seam) that drives script systems from the fixed-step
  tick.
- Contracts:
  - Determinism: script tick code sees fixed-timestep state only — no
    wall-clock, no hash-order iteration inside the tick.
  - Snapshots: only the C++ `Registry` + `RngStream` serialize; script handles
    stay transient and rebind on load.
  - Main-thread affinity; per-call `error_code`/`expected` containment — script
    errors fail closed to the call and never throw to `requestExit`.
  - Allow-list sandbox: no FS/process/clock access from script; scripts emit
    intent components only, never `DrawSubmission`/`Camera`.
- Interaction: script config arrives as a typed `Desc` at init (§1.5); dev
  hot-reload rides the §2.5 watcher story (scripts reload like assets dev-only,
  pack-only shipping fail-closed); editor gets a script-debug toolwindow (§4.1).
- Sequencing: after ECS (§1.4) + config decision (§1.5), before §5 gameplay
  systems; prototype on tools/AI goals first (errand effects, goal scoring)
  before opening any mod-facing surface.

## 2. Assets direction

Authoritative base spec: `docs/plans/engine.asset-content-pipeline.md`
(XNA asset = path + importer + processor, vend-only caller-owned `IContentService`,
single JSON manifest, `.ppak` shipping, host-only cooker, P0–P4). Everything here
is a delta on top of that spec, not a replacement.

### 2.1 New `engine.asset` module with cooking/packing (already designed)

- Implement per the existing spec; this doc adds only downstream consumers:
  audio assets (§2.4), hot reload (§2.5), editor asset library (§4.3).
- Non-goals from that spec stand: no generic importer extensibility, no DDC,
  no async service calls in v1.

### 2.2 Quaternion tangent space (`float4` instead of N/B/T)

- Merge normal + binormal + tangent into one quaternion `float4` in
  `StaticMeshVertex` (replacing the current `float m_normal[3]` + `float m_tangent[4]`
  layout), shrinking the vertex DTO and simplifying the `BagVertex` Slang mirror.
- Consequences to handle explicitly: vertex `static_assert`s (size/stride/offsets),
  CPU↔Slang mirror table, MikkTSpace fallback path, tangent-`w` mirror probe,
  pack wire versioning (payload kind + per-kind version in TOC `kind` field),
  cooker parity (dev loose vs. shipping pack produce identical frames).
- Sequencing: fold into the mesh `:serialize` work (P1 in the asset spec) rather
  than as a separate migration — cheaper to version the wire format once.

### 2.3 Scene nodes as TRS decomposition, not raw `float4x4`

- Replace `SceneNodeAsset { m_local, m_world }` raw matrices with
  `{ translation float3, scale float3, rotation quaternion float4 }` (+ computed
  world on load), matching the glTF TRS authoring model and the row-vector
  `S*R*T` convention already documented in the asset plan.
- Why: cheaper interpolation/retargeting for agents and building placement,
  unambiguous scale/mirror handling, smaller pack payloads, editor gizmo (§4.2)
  edits TRS directly instead of decomposing matrices lossily.
- Keep both local and world (world accumulated `parent*local` at convert);
  instance model matrices are still composed `float4x4` at submit time for the
  existing `MeshPush`/`InstancePayload` shader path.

### 2.4 External audio library + audio asset type

- Add an external audio decode/playback library (selection TBD — needs the same
  PRIVATE-dep discipline as mango-image/mango-import3d: never exported across
  the module boundary) and a new `audio` asset type through the manifest →
  importer/processor → pack → `loadAudio` handoff path.
- Explicit non-goal carried over: v1 of the asset spec has "no audio" — so audio
  arrives as a typed second wave with its own `AudioOptions`, pack payload kind,
  and cooker support, following the exact shape of the image/mesh precedent.
- Runtime playback (mixer, 2D positional for flat-land, UI feedback) is an
  `engine.app` service consuming vendored audio bytes — same caller-owned
  pattern as images/meshes, never service-owned handles.

### 2.5 Hot reloading through the content pipeline

- Dev-only: manifest watcher (`DirectoryWatcher`/`IoPort` from `engine.core:io`)
  → re-import/re-process changed source → `load*` again + re-upload, same
  "dev reload = load again + re-upload, no `reload()` API" rule the asset spec
  already sets.
- Scope: shaders first (fastest iteration win for §3 work), then images/meshes,
  then audio. Generation tracking on `SharedModule` views (already planned in
  the asset spec §7) is the correctness hook: stale views must assert/fail
  closed, never silently dangle.
- Shipping stays pack-only, fail-closed, no watcher, no loose fallback.

## 3. Rendering direction

Target: a true 2D-flat-land shading pipeline — physically-inspired BxDF shading,
realtime GI for enclosed bases, TSR for crispness, filmic post, GPU-driven scene,
and the unified particle substrate as both simulation and rendered matter.

### 3.1 True shading pipeline with BxDF (BRDF/BSDF) framework

- Replace ad-hoc lambert/factor shading in `mesh_bindless.slang` with a BxDF
  framework: material parameter evaluation (albedo, metallic, roughness,
  occlusion, normal) → BxDF lobe selection → light accumulation → post chain.
- Keep the established GPU vocabulary: `GpuMaterial` StructuredBuffer mirror,
  bindless textures + one shared sampler, `kNoTexture` fallbacks, pipeline-key
  variants (opaque/mask now; blend/transmission only with a specified sorting +
  depth policy). See `slang-shader-developer` skill for Slang/RHI binding rules.
- Sequencing: first rendering milestone; GI (§3.2), post (§3.4), and GPU scene
  (§3.5) all plug into the lobe/light interfaces defined here.

### 3.2 Holographic Radiance Cascades for realtime GI in flat-land 2D

- Implement HRC-style realtime global illumination for the 2D colony view
  (enclosed rooms, lamps, glowing machines, lava/fire emissives), computed in
  world space — with an irradiance-cache-in-world-space variant under evaluation
  specifically because the CPU gameplay needs it too (agents avoid darkness,
  plants grow by light level, thermal/light comfort queries).
- Key design decision to record early: exact world-space cache layout, CPU readback
  path + staleness contract (gameplay reads last-finished cascade, never a
  half-written one), resolution vs. 4096² world scaling (tile/chunk streaming,
  sleeping bricks like the particle sim's §8 constant-cost idea).
- Sequencing: after §3.1 (needs BxDF hooks for indirect); validate on a
  two-room + corridor + lamp scene before touching the full colony.

### 3.3 Multisampling Temporal Super Resolution

- Per https://filmicworlds.com/blog/temporal-super-resolution-via-multisampling/:
  MSAA-sample-jittered history resolve for crisp edges + stable TSR upscale on
  the flat-land scene and particle matter.
- Prerequisites: render-thread command ordering (§1.2) fixed, velocity/output
  motion vectors from the camera + ECS transforms, history-buffer lifetime tied
  to the fence/scoping rules (no use-after-release across level transitions).
- Sequencing: after §3.1, in parallel with §3.2; both feed the post chain (§3.4).

### 3.4 Post-process chain: luma-histogram auto-exposure, bloom, AgX

- Chain: luma histogram → auto-exposure → bloom → AgX tonemapping (→ UI composite).
  Histogram lives on GPU; exposure is a smoothed uniform, not a per-pixel guess.
- Keep passes content-specific (each pass owns its shader/pipeline/buffers) and
  `Renderer` content-free per contract. Each stage gets a validation scene
  (dark cave + lamp flare + daylight cut) with GPU timings per pass.
- Sequencing: after §3.1; exposure constants feed back into §3.2 GI tuning.

### 3.5 GPU scene: culling + multi-draw-indirect

- Move the per-(primitive, instance) CPU `drawInstanced` loop toward GPU-driven:
  frustum/tile culling in compute, compacted multi-draw-indirect args buffer,
  payload capacity clamping already established in the bindless plan.
- Flat-land advantage: 2D camera bounds make culling intervals cheap; chunk the
  4096² world (§5.1) into tiles that map 1:1 to cull groups and HRC cache tiles
  (§3.2) and particle sleep bricks (physics spec §8).
- Sequencing: after §1.2 (threading) + §3.1 (shading payload stable). The deleted
  L1/L2b indirect lanes in the old asset-pipeline draft stay deleted; this is a
  fresh design on the shipped CPU-staged path.

### 3.6 MPM-PBD particle simulation for gas/liquid/solid/semi-solid

- Per https://media.contentapi.ea.com/content/dam/ea/seed/presentations/seed-siggraph2024-pbmpm-paper.pdf
  (PB-MPM) as material-modeling reference, implemented on the architecture in
  `docs/physics/unified_particle_simulation.md`: one particle-parcel substrate,
  SPH-style density + PBD/XPBD correction first, discrete LOD levels with
  split/merge + hysteresis, sleep bricks, gas/temperature/phase-change/granular/
  solid/chemistry phases 0–14 with gates.
- Gameplay bindings (§5.8 tools, §5.9 fire, water/air networks): particles are
  spawned/dug/exploded/dragged/repulsed/attracted through ECS systems; rendered
  through the §3.1 shading path as shaded matter, not debug points.
- Hard rules from the physics spec: mass/momentum/material/volume conservation
  invariants, no PB-MPM/FLIP/Poisson/dense-world-grid until a phase calls for it,
  validation scenes + per-pass GPU timings with every phase, gates block progress.
- Sequencing: the long pole. Start Phase 0 (infrastructure + neighbor correctness)
  as soon as §1.2 threading answers "which thread owns particle buffers";
  gameplay needs only Phase 1–2 (liquid) + Phase 9 (gas) + Phase 10 (temperature)
  for the first playable water/air/heat loop.

## 4. GUI direction (editor)

### 4.1 Editor toolbars + toolwindows

- Layout: toolbars (tools, play/pause/step, layer visibility) + toolwindows
  (camera controls + properties, selection properties, input contexts from §1.3,
  asset browser from §4.3, network overlays from §5.4–5.6, thermal overlay from
  §5.7, particle stats from §3.6, AI plan debugger from §5.10, physics/coupling
  overlay from §5.11).
- `ApplicationEditor` keeps owning scene/player/camera/viewport/UI state;
  toolwindows are views over that state, never owners. ImGui integration stays
  in `engine.app:ui`; overlay shader stays embedded, not an asset.

### 4.2 Mouse picking + selection gizmo

- Picking: CPU raycast against TRS scene nodes (§2.3) for buildings/agents +
  cell pick against the 4096² grid (§5.1); hover highlight, click select,
  translate/rotate gizmo editing TRS directly with typed numeric entry.
- Gizmo edits flow through ECS commands so sim, renderer, and audio observe one
  consistent change stream (no direct renderer mutation from UI).

### 4.3 Asset library window + content-pipeline registry

- Build on the ImGui asset-window sample: a global-registry browser over the
  manifest-declared assets (name, type, source, tags, deps, options, cook state,
  pack membership, hot-reload status from §2.5).
- The manifest JSON stays the authoring truth; the window edits staging/selection
  and validates (duplicate names, dangling deps, tag coverage) — it never becomes
  a second truth store beside the manifest.

## 5. Game direction: ONI-like colony sim

Fantasy: procedurally generated flat-land asteroid interior; crew of duplicant-like
agents build, pipe, wire, farm, and survive heat, vacuum, floods, and fire. Systems
over script: every network sim exposes state to agents, tools, and the HRC light
cache alike. Scope is deliberately ONI-shaped — agents/entities + AI, collisions,
power/water/air/heat/pathfinding/buildings/tools/fire — because those systems
interact to produce stories.

### 5.0 Sim foundation: chunks, tick order, snapshots (build this first)

All of §§5.1–5.11 stand on three shared decisions. Set them before world gen
lands — re-choosing any of them later re-chunks every consumer.

- Chunk layout (one geometry for every system): 4096² world as 32×32 chunks of
  128² cells. Chunks are the unit of activity (dense-active vs. sparse-quiescent),
  dirtiness (dig/build/tool edits mark chunks dirty), and sleep/wake (one wake
  protocol shared by grid, networks, Box2D bodies, particle bricks, HRC tiles,
  and overlay tiles). Pathfinding (§5.2), colliders (§5.11), GI tiles (§3.2),
  particle bricks (§3.6), and GPU overlays (§4.1) all align to this grid — no
  second tiling per system. Cold/quiescent chunks cost ~nothing per tick; tools,
  edits, flows, contacts, and refinement radii wake exactly the chunks they touch.
- Tick order (fixed step, declared once): tool/edit commands → grid + phase
  changes (§5.1) → networks power → water → air → thermal (§§5.4–5.7, declared
  deps, no back-edges within a tick) → Box2D step (§5.11) → particle step (§3.6)
  → explicit coupling pass (bounded, clamped both ways) → AI planning slices +
  agent movement (§§5.10, 5.2) → render/GI read-only feedback. No system reads
  a half-ticked neighbor; cross-system state from tick N is visible at tick N+1.
  Each system gets a per-tick budget: overruns slice (planning, pathfinding,
  collider rebuilds) or defer (GI tiles, overlays) — never spike. Sim uses only
  seeded per-system RNG + the injected clock (§1.1); wall-clock and thread timing
  never enter sim state, so ticks are deterministic.
- Snapshot contract (save/load + debug): versioned snapshot = seed + grid chunk
  deltas + ECS components + Box2D bodies + planner/replan queues + per-system RNG
  streams. Roundtrip rule: save → load → next tick is bit-identical to the
  uninterrupted run. Planner and pathfinding queues are state, not caches — they
  serialize. Snapshot version bumps with any component/cell/pack-layout change;
  old versions fail closed with a clear error, never silent migration. The editor
  pause/step/speed control (§4.1) rides this contract: stepping is N deterministic
  ticks from a snapshot, editing while paused marks chunks dirty without ticking.

### 5.1 World: 4096×4096 element grid, biomes, temperature, atmosphere

- One 2D grid, 4096×4096 cells; each cell holds an element (vacuum, rock types,
  ores, water, polluted water, steam, oxygen, hydrogen, chlorine, …) plus
  temperature + pressure/mass fields.
- Procedural generation: biome regions (temperate core, frozen edge, caustic
  pocket, oil/low reservoir, ruins), carved caverns + abyssal border, embedded
  geysers/ruins/points of interest. Deterministic seed; regeneration from seed
  must reproduce the world.
- Performance contract: cost follows active complexity (sleeping bricks/chunks
  like the particle spec), not 16.7M cells per tick. Static/settled regions go
  quiescent; tools, networks, and particles wake them.

### 5.2 Pathfinding: ladders, hurdles, shortcuts, abilities

- Grid pathfinding over walk/climb/jump/fly capability sets: solid floors,
  ladder shafts, jumpable hurdles (1-cell gaps/ledges), doors (permission +
  power states), shortcuts agents learn, flying vs. walking movement models.
- Must degrade gracefully: partial paths (dig-to-here, build-to-here errands),
  replanning on world edits without frame spikes (budgeted async or chunked
  planning on the sim thread), debug overlay showing paths + blockers.

### 5.3 Base buildings: walls, ladders, doors, furniture, machines

- Catalog v1: walls/tiles, ladders, doors (manual/powered), beds, tables, chairs,
  lamps (light sources for §3.2 GI + comfort), plus machine sockets for §5.4–5.6
  (generators, batteries, pumps, filters, purifiers, vents, turbines, taps).
- Buildings are ECS prefabs with TRS placement (§2.3), cell footprint, material/
  flammability tags (§5.9), power/water/air port declarations for the network
  sims, and work errands (build, repair, demolish) for agents.

### 5.4 Power network: cables, switches, timers, sensors, generators, batteries

- Conductive-network sim: producers (coal/hydrogen/solar-equivalent, turbines
  from §5.5 water flow), storage (batteries with charge curves), consumers with
  priority, cables with capacity + overload breakage, switches/timers/sensors as
  logic gates (time, pressure, temperature, occupancy thresholds).
- Failure stories: overloads spark fires (§5.9), blackouts stall pumps/filters,
  batteries leak heat (§5.7). Overlay in editor (§4.1) shows flow + overload risk.

### 5.5 Water network: pumps, pipes, filters, turbines, sanitation loop

- Loop: pumps → pipes → filters → consumers (taps, farms, machines) → toilets/
  baths emitting polluted water → purifier → clean return; turbines harvest flow
  energy back into §5.4; bottler produces portable water for errands.
- Polluted vs. clean is a first-class element distinction (ties to §5.1 grid +
  §3.6 particle phase when spilled). Clog/pressure/backflow rules must be simple
  enough to debug in the overlay, rich enough to flood a base when mismanaged.

### 5.6 Air network: pumps, ducts, filters, vents

- Pressure/flow sim over duct graphs + room diffusion into §5.1 atmosphere cells:
  pumps pressurize, ducts route, filters scrub (CO₂/pollutants), vents release.
- Ties: vacuum suffocates, hydrogen rises, chlorine sinks, heat rides airflow
  (§5.7), fire breathes (§5.9). Gas-coarsening behavior from the particle spec
  (Phase 9) is the perf model: homogeneous rooms stay cheap, doors/fires refine.

### 5.7 Thermal conductivity simulation

- Per-cell/per-building temperature + conductivity: conduction between neighbors,
  insulation values on walls, heat from machines/power (§5.4), turbines, fire
  (§5.9), magma-edge biomes; cooling via vents/water loops (§5.5–5.6).
- Phase changes (§5.1 elements, particle spec Phase 11) ride temperature:
  water↔steam, polluted water↔steam, crude→refined chains. Expose temperature in
  the §3.2 world-space cache family so gameplay and GI share one spatial truth.

### 5.8 Player tools: spawn/dig/particles/explode/temperature/force

- Toolset: spawn elements, dig cells, spawn gas/liquid/semi-solid (snow/sand)
  particles via §3.6, explode/teardown cells into particles, paint temperature,
  drag/repulse/attract particle fields.
- Tools are the physics-debug UI too: every tool shows its wake/refinement radius
  (predictive refinement before explosions/fires per the physics spec §36),
  conservation counters (mass/momentum deltas), and affected-chunk highlighting.
- Input model depends on §1.3 (context + chord + priority) so dig/paint/drag do
  not fight camera controls or selection (§4.2).

### 5.9 Fire simulation + burnable buildings

- Fire as reaction (fuel + oxygen + heat, cf. particle spec Phase 14 chemistry):
  burnable building/material tags, spread by adjacency + airflow (§5.6) +
  temperature (§5.7), outputs heat + smoke/CO₂ (back into §5.1 atmosphere),
  extinguishable by water particles, vacuum, or demolish-firebreaks.
- Power overloads (§5.4) ignite; water loops (§5.5) suppress; airlocks starve.
  The loop closes: every network sim both feeds and fights fire.

### 5.10 Entities: GOAP gameplay AI on top of ECS

- Agents (duplicant-like crew), critters, and workable machines are ECS entities;
  behavior is goal-oriented action planning: goals (eat, breathe, sleep, build,
  repair, flee fire/vacuum) + actions with preconditions/effects/costs, planner
  (A*-over-action-graph, STRIPS-style) producing plans that consume §5.2 paths,
  §5.3 errands, and network/world state (§5.1, §5.4–5.7) as sensors.
- Planner runs budgeted on the sim thread (plan one agent per N ticks / plan
  slices, never a full-colony replan spike); plans invalidate on world edits
  with graceful fallback (replan, then idle-seek, never freeze). Needs/morale
  curves stay data (components), never hidden planner state, so save/load and
  the editor AI-debugger toolwindow (§4.1) observe one truth.
- Survey (not decided): no C++ GOAP library dominates the way Box2D does physics,
  so expect survey-then-probably-own. To survey:
  - GOAP references: Orkin FEAR-style STRIPS planner, ReGoap (C# reference design),
    GOAPLite-class minimal planners — for the precondition/effect/cost + A* shape;
  - HTN alternative: Fluid HTN concept / hierarchical-task-network planners —
    often more authorable than flat GOAP at ONI errand scale; evaluate GOAP vs.
    HTN vs. hybrid before committing;
  - BT comparison: BehaviorTree.CPP — as dispatch/execution layer under or beside
    the planner (planner picks the goal chain, BT executes it robustly), and as
    the fallback if GOAP authoring cost proves too high;
  - ECS fit: planner state as components on the EnTT-candidate registry (§1.4) —
    survey must answer replanning cost at colony scale, determinism under
    fixed-timestep (§1.1), and snapshot/serialize for save/load.
- Sequencing: after ECS (§1.4) + pathfinding (§5.2) + buildings/errands (§5.3);
  first playable needs only seek/work/flee goals — full needs simulation later.

### 5.11 Collisions: rigid bodies + static grid, cohabiting with particles

- Physics scene emerges from ECS + grid: solid cells (§5.1) mesh into chunked
  static colliders (dirty-chunk rebuild on dig/build, never per-cell bodies),
  buildings contribute static/kinematic shapes, agents/items/crates are dynamic
  rigid bodies, ladders/doors/vents are sensors. All collider state derives from
  ECS components — the physics world is a view, never a second truth store.
- Main candidate: Box2D v3 (2D rigid/static/kinematic, sensors, continuous
  collision, deterministic stepping that fits §1.1 fixed-timestep). To survey
  before committing: Box2D v3 vs. box3d (the only 3D alternative under
  consideration, and only if 2D-in-3D ever has a future) vs. other 2D engines
  (Chipmunk2D / Rapier2D-class) specifically for grid handling — favor solutions
  that query the grid directly (tile/grid colliders, chain/edge extraction,
  custom broadphase over cells) over ones forcing a generated static mesh per
  chunk — on determinism, rebuild cost at 4096² edit rates, sensor density
  (doors/ladders), and C++20-module/private-dep cleanliness.
- Cohabitation with the particle simulation (§3.6) is the high-risk design point
  and must be designed jointly, not bolted on:
  - Rigid bodies act as kinematic colliders inside the particle solver (velocity
    boundary conditions), never as second-integrated copies drifting apart;
  - Particles feed back (buoyancy/drag/pressure integrals) through one explicit
    coupling pass per tick with bounded force clamps — no hidden per-substep
    messaging that breaks determinism or teardown order;
  - Sleeping/split-merge (physics spec §§8, 4–5) must wake or refine on rigid
    contact, and Box2D bodies must sleep when their chunk sleeps — two sleep
    systems, one wake protocol.
  - Coupling survey: FleX-style unified particle/rigid coupling, EA SEED PB-MPM
    rigid-coupling sections, XPBD rigid-vs-particle contact handling, Box2D
    contact-listener → particle-force feedback patterns. Prototype coupling on a
    dam-break-vs-crate + landslide-vs-wall scene pair before colony scale.
- Sequencing: physics scene right after world gen (§5.1) + ECS (§1.4) — agents
  (§5.10) and tools (§5.8) both stand on it; coupling design freezes before
  particle Phases 9+ bind gameplay forces.

## 6. Suggested build order (dependency-aware, not a schedule)

1. Frame pacing + diagnostics (§1.1) — measures everything after.
2. `engine.asset` v1 per its spec (§2.1) + TRS nodes (§2.3) + quat tangents (§2.2)
   folded into its mesh work — unblocks shipping packs and all asset consumers.
3. Render thread + channel (§1.2) and input state machine (§1.3) in parallel —
   threading fixes GPU-scene/particle ownership; input unblocks tools.
4. ECS substrate (§1.4) + script engine leaf (§1.6) + audio asset (§2.4) + hot reload (§2.5) — gameplay glue
   and iteration speed before content volume.
5. BxDF shading (§3.1) → post chain (§3.4) → HRC GI (§3.2) + TSR (§3.3) →
   GPU scene (§3.5) — each stage validated with timings before the next.
6. Particle Phases 0–2 + 9–10 (§3.6) — first playable fluids/gases/heat.
7. Editor GUI (§4) grown alongside 5–6 — toolwindows arrive with the systems
   they inspect (networks, particles, assets).
8. Colony sim systems in playability order: sim foundation (§5.0: chunks, tick
   order, snapshots) → world gen (§5.1) → collision scene
   (§5.11, static-chunk + Box2D survey) → pathfinding (§5.2) → buildings (§5.3)
   → entities/GOAP AI (§5.10, seek/work/flee first) → power (§5.4) → water (§5.5)
   → air (§5.6) → thermal (§5.7) → tools (§5.8) → fire (§5.9). Each network ships
   with its overlay + debug scene before the next begins; rigid↔particle coupling
   (§5.11 × §3.6) is prototyped on dam-break-vs-crate before colony scale.

## 7. References

- `docs/plans/engine.asset-content-pipeline.md` — asset module constitution.
- `docs/physics/unified_particle_simulation.md` — particle matter constitution.
- https://media.contentapi.ea.com/content/dam/ea/seed/presentations/seed-siggraph2024-pbmpm-paper.pdf — PB-MPM material reference.
- https://filmicworlds.com/blog/temporal-super-resolution-via-multisampling/ — TSR via multisampling.
- `AGENTS.md` — module graph, ownership/lifetime/teardown, renderer/camera, test
  and formatting contracts everything above must respect.
- `codemap.md` + per-directory `codemap.md` — where each piece lives today.
