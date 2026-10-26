# game/colony/

`MatterRuntime` uses resident-only owned requests. Completion returns compact status and a
strong versioned presentation view; metadata preparation is inventory admission
work, while each acknowledged tick copies resource/identity values in O(1).
Snapshot begin/poll/end retains the runtime freeze after GPU collection until
the caller finishes demand reconstruction. GPU edits prepare an appended suffix,
poll its copy fence, then publish through the same active solver and operation
slot. The driver selects the resident route and publishes its O(1) view only
after the GPU and dependent Scene step jointly commit. Normal ticks retain
physical/ID/gas metadata, with no full particle readback, restore, coordinate
sweep, presentation batch conversion, or particle CPU upload.

## Asynchronous Matter ownership

`MatterRuntime` retains one instance/generation-qualified request, its owned
inputs/result and one ticket on the existing RenderLane operation slot. Named
begin operations retain inputs once; `advance` distinguishes transport pending
from callback completion and from a completed poll that has no GPU candidate.
Accounting/failure are last-acknowledged copies. Inline/headless fixtures drive the same begin/advance protocol; synchronous
Runtime compatibility methods and nonresident mode bookkeeping are removed.

Driver retains its submitted tick prefix and compact validated status until
publication acknowledges. Body-source state and coupling epoch still validate
before publication; one-way Scene commit records the frontier without impulses
or wake, then steps Box2D once. Edit preparation and
joint publication use the same retained request. State-preserving detach first
settles already-mutated work; prepared-world cancellation retains the candidate
through its own acknowledgement. WorldJob holds prepared CPU ownership during
these device operations and releases it before worker cleanup. Checkpoint
capture continues to require a settled tick and detached CPU owner. Demand
capture freezes tick/edit admission at the joint publication, copies current
GPU rows, and reconstructs/validates all carried fields on the injected existing
chunk scheduler. The pool must outlive the driver and its owned task. WorldJob
retains exclusive main ownership through reconstruction, records body awake
values on main, then detaches before its capture worker can read the driver.
The active solver remains Runtime-owned, so retained UI/rendering continues.
Cancellation drains admitted copy/reconstruction before returning the CPU owner.
`captureParticles`, `particleChunk` and `captureMatterState` reject unavailable
coordinates after evolution; `matterState` returns null. Snapshot tick and
availability are explicit. Terrain edits request a current snapshot for the
existing exact double coverage check, copy old rows GPU-to-GPU, and upload only
new deterministic-ID rows before the copy-fence/joint publication boundary.

Named descriptor addition: `Content.Descriptors.cpp` implements the owned
`ContentDescriptorCatalog` declared in `Content.cppm`. Composed schemas reuse
sim definitions and existing appearance metadata, pin immutable pack metadata,
and validate true cross-domain asset links and explicit palette index domains.
`Content.Descriptors.Tests.cpp` exercises the real two-root module/config seam,
shared Elements, missing/corrupt binary sources, owned provenance and symlink
containment. `loadContentCatalog` returns this all-World definition catalog;
binary texture preparation is a separate explicit request.

`Content.Assembly.cpp` assembles one selected World from its exact palettes,
rejects duplicate asset targets, composes shared Elements into distinct numeric
Materials, and lowers through the sim compiler's explicit bindings. Descriptor
provenance maps numeric alias diagnostics back to the owning file with selected
World context. Numeric identity excludes asset linkage names and appearance;
presentation descriptor identity includes selected appearance and texture
settings, with source-byte identity deferred to explicit image preparation.
`game/world_assembly` covers this seam and shipped definitions-only parity.

`WorldGen.Capture.cpp` implements the `game.colony.smoke` blocking GPU diagnostic
entry point. The invocation boundary passes an exclusively owned initialized
authored driver and reproduction/camera values; no simulation ticks run during
offscreen capture. Actual GridPass RGB readbacks and camera manifests complement
the flat CPU exports. This is separate from interactive candidate publication.

## Responsibility

Game-owned colony simulation, tools, presentation translation and headless workloads. The default world is seeded and uses `engine.sim` geometry. Physics is coordinated by the game driver, not imported into sim or app.

`MatterRuntime.cppm/.cpp` (`game.colony.matter`) is the game-owned execution
adapter to the sole `engine.matter` GPU solver. Copied requests and results cross
the existing RenderLane owned-operation slot; inline headless ownership uses the same
operations. CPU metadata belongs to Driver; complete `MatterState` values exist
at admission and explicit current snapshots, including unsimulated gas. Their
publication epoch is independent of a live solver's resource epoch.
`ColonyDriver.Matter.cpp` owns preparation/attachment and the pending final physics
phase; an admitted prefix runs once, completion jointly commits GPU/Box2D, and
paused catch-up slices remain held. Joint terrain
edits prepare carried particle resources and the collider cohort before either
publishes; rendering consumes the resulting dirty revisions independently.
WorldJob transfers only detached exact CPU owners. Live native presentation and
paired production performance remain separate executable acceptance gates.

## Module navigation

| Module | Files | Responsibility |
|---|---|---|
| `game.colony` | `Colony.cppm/.cpp` | RAII grid ownership and generation lifecycle |
| `game.colony.world_progress` | `WorldProgress.cppm` | Shared world-work state/stage/read-only progress vocabulary, independent of the application host |
| `game.application:world_job` | `WorldJob.cppm/.cpp` | Asynchronous candidate preparation, cancellation, pointer publication and retirement; optional owned per-request Prepare overrides reuse the default worker/context/cleanup pipeline |
| `game.colony.worldgen` | `WorldGen.cppm/.cpp`, `WorldGen.Plan.cpp` | Authored numeric-plan preparation |
| `game.colony.worldgen_preview` | `WorldGen.Preview.cppm/.cpp`, `WorldGen.Preview.Export.cpp` | Bounded biome samples and exact crops of cached production worlds, canonical initial particles, diagnostic/source export |
| `game.colony.worldgen_panel` | `../ui/WorldGenPanel.cppm/.cpp` | Merged authoring workspace, embedded stable-key browser, all-document property controls and preview/Start/Return/Quit intents; no I/O or renderer mutation |
| `game.colony.content` | `Content.cppm/.cpp`, `Content.Source.cpp`, `Content.Descriptors.cpp`, `Content.Assembly.cpp`, `Content.Presentation.cpp` | Owned multi-pack sources and definition-only importing; all-World descriptor catalog; direct selected-World numeric projection; separately prepared presentation through the shared asset request. `decodeWorldGenDefinition` handles the separate version-2 definition envelope. |
| `game.colony.metric` | `Metric.cppm/.cpp` | Checked 3:1 fine-cell/metre conversions, globally anchored construction footprints, nominal volume and persisted metric compatibility preflight. Authored sessions use this metric for actors, colliders, tools and rendering. |
| `game.colony.driver` | `ColonyDriver.cppm/.cpp` | Explicit-time fixed step, pause/step/speed/regenerate, ECS and driver-local physics |
| `game.colony.agents` | `Agents.cppm/.cpp` | Agent state, planning and movement vocabulary |
| `game.colony.buildings` | `Buildings.cppm/.cpp` | Footprints, progressive build/demolition errands, component registration |
| `game.colony.pathfinding` | `Pathfinding.cppm/.cpp` | Bounded resumable BFS over vacuum cells |
| `game.colony.digtool` | `DigTool.cppm/.cpp` | Rectangular dig edits |
| `game.colony.sim_packet` | `SimFramePacket.cppm/.cpp`, `SimFramePacket.Diagnostics.cpp` | Frozen diagnostic maps, immutable particle translation cache, owned simulation snapshots, bounded initial presentation batches, confirmed-prefix cursor, coherence checks and grid staging |
| `game.colony.translator` | `ColonyTranslator.cppm/.cpp` | Resident-chunk translation/cache revisions, presentation invalidation |
| `game.colony.panel` | `../ui/ColonyPanel.cppm/.cpp` | Prepared presentation values/state/intents; no driver/renderer/service access while drawing |
| `game.colony.smoke` | `ColonySmoke.cppm/.cpp` | Headless readback smoke |

Prepared simulation packets own numeric terrain visuals, catalog identity and
the fine-cell metric. Terrain uploads preserve sparse catalog IDs and vacuum
zero; standalone packetless grid fixtures retain their transparent sentinel. The driver
exposes a borrowed immutable `contentCatalog()` view, copied at packet capture.
The render lane never borrows the live catalog. Authored texture periods are
bounded for finite float UV arithmetic before generation.

Aggregate `Game.SimPacket.Presentation` and nested `Game.SimPacket.Catalog`
profiling scopes isolate packet presentation capture and its combined image/
material/background translation batch. They do not measure image copying alone
or change packet ownership/caching.

Check each `export module` declaration and `../CMakeLists.txt` for exact module names/registration. `Colony.Elements.h` owns element IDs (vacuum is 0).

## Simulation and presentation contracts

`Content.Source.cpp` owns complete manifest/descriptor source sets and immutable
checked snapshots. Snapshots pin one pack index; the synchronous required reader
imports their owned bytes without disk fallback. Text budgets and physical path
ownership are checked before importing. `Content.Presentation.cpp` composes
selected appearance with image preparation; its sidecar owns completed images
and identifies the selected catalog and request generation.

`Content.Authoring.cppm/.cpp` (`game.colony.content.authoring`) owns multiple
explicit pack roots, a complete desired source set, a separate per-file disk
baseline, grouped command history, compile parameters, and the last validated
definition catalog. Invalid typed drafts remain editable. Snapshot workers never
borrow the mutable session. Draft/save validation checks every declared World
through the already imported descriptors without binary texture work.

`saveContentDraft` checks all owned files for external conflicts, writes
descriptors before publishing manifests, and removes obsolete files only after
manifest success. Individual replacements are atomic; the pack transaction is
not. Partial outcomes retain observed disk successes while preserving newer
draft/history intent. Private identity-bound restoration proofs support undo of
saved deletions; orphan files never enter complete imported candidates.
Explicit reload adopts external changes through a complete replacement session.
Only matching full success publishes the validated catalog. Concurrent external
writes during commit are unsupported.

`ColonyDriver` owns Colony, ECS, fixed-step accumulator, physics Scene and ChunkColliders. Default tick is 60 Hz; physics uses fixed 1/60 s. The declared steps are tool, build, pathfind, aiplan, movestep and physics. Simulation consumes explicit elapsed time; wall-clock profiling does not enter simulation state.

Main captures `SimFramePacket` after advancing simulation. `makeSimFramePacket` copies simulation identity/counters/chunks; `isSimPacketCoherent` checks tick/world-generation, and `buildGridStagedFrame` uses owned packet data. The interactive render worker never reads mutable driver/grid/translator state. Edit revision manifests round-trip with lane results; only matching consumed payloads acknowledge pending changes. Failed/dropped frames must not silently lose edits. See [game flow](../codemap.md) and [frame transport](../../lib/engine/app/frame/codemap.md).

Initial presentation distinguishes successful CPU staging/reset from completed
atlas publication. A matching staged reset is latched once across deferred
retries; the visible prefix and dirty acknowledgments still advance only after
successful publication and presentation.

Translator's submitted/resident counts are not frustum-visible counts. Cache invalidation and sim dirty state are separate concerns; do not clear simulation dirt merely because a descriptor was submitted. Debug triangle instances for agents, paths, grid and edit feedback are actively staged alongside grid tiles.

`ColonyPanel` lives in [game UI](../ui/codemap.md), consumes prepared views and returns intents/chrome; the main client applies actions. Display draft/persistence policy lives in `../Game.Display.*`. Pathfinding/build bounds and diagnostic row inventories live in their declarations; avoid duplicating stale numeric inventories here.

Display controls and telemetry render through `engine.app:ui.widgets.display`.
The colony panel retains draft synchronization and adapts widget actions to game
intents; game code retains validation, transactions, confirmation timing and persistence.

`ColonySmoke` supplies `runColonySmoke(SmokeOptions)` and verifies deterministic simulation contracts plus offscreen GPU readback. Platform profiler markers remain available for performance investigations.

Candidate initialization accepts explicit generation execution dependencies and a publication epoch. Spawn searches, body preparation and collider chunks check the job context; actor spawning uses the requested seed before publication. World replacement adopts a separate authored session; there is no seed-only generator or in-place regeneration API.

`WorldGen` also exposes a factory-only `PreparedWorld`, owning the immutable
catalog, reproduction `WorldDescriptor` and complete numeric `GeneratedWorld`.
Its factories accept an explicit clock and execution dependencies; the path
overload loads the catalog on its calling worker. `Colony::install` adopts this
owner without rerasterizing, while `grid()` preserves the existing edit boundary.
`ColonyDriver::initPrepared` adopts it into an isolated candidate with metre-scale
shared-contour collision and exactly one controlled actor at its validated start.
The interactive job captures candidate presentation off-main; CPU simulation
replacement publication follows old-driver snapshot/detach, candidate solver
preparation, exact graphics activation ACK, then acknowledged Runtime attachment.
The old active Runtime owner survives reversible preparation. Host publication
transfers once before retiring the detached old CPU owner; actual resident
presentation separately releases readiness while retaining the presentation header. Generated-session
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
configured authored pack. Authored initialization requires one actor.
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

`ColonyDriver.Edits.cpp` owns per-chunk rollback candidates, suspended tick prefixes
and bounded collider preparation. Attached matter commits only after both solver
resources and the collider cohort are prepared; no render acknowledgment is
required. Removed catalog solids emit one source-sand sample per cell using the
original material ID, catalog density, actual cell temperature and nominal
cell-area times reference thickness. Existing full carried columns and gas are
preserved. Added solid contours defer the entire uncommitted cohort only where
they newly cover a live moving/gas/emitted point; the exact shared triangle
boundary counts as inside. The copied notice is consumable by the host, and a
later fixed tick retries while physics may move the obstruction. Recoverable
preparation failure retains the candidate; unexpected failure after either
irreversible publication is terminal. Legacy unattached fixtures retain their
explicit static edit path; attached no-catalog edits report `not_supported`.
Pause/single-step preserve held tick accounting and presentation catches up from
the committed revision manifest.


`Checkpoint.cppm` / `Checkpoint.cpp` / `Checkpoint.Codec.cpp` /
`Checkpoint.Validation.cpp` own mandatory current-state game envelope v5. It
stores every scalar particle column (p/d/D/F/J/lambda/logJp/color/enabled), the
fixed profile, physical ledger/gas/next ID, metric and durable publication/terrain
epochs. Older envelopes fail with `protocol_not_supported`; decode never seeds
missing columns. Capture copies existing state, or explicitly initializes source
defaults from the current physical ledger only for a never-materialized owner.
This does not retroactively identify earlier CPU-only ticks as PB-MPM. Restore
rebinds only world identity, validates profile/units/material mapping and rebuilds
cold Box2D state. Its continuation guarantee is restored-versus-restored, not live
Box2D cache parity. WorldJob retains the exclusive frozen-owner capture protocol.
Raw sim snapshot v4 and asset manifest v1 remain separate formats.


`WorldGen.Export.cpp` streams bounded flat PPM overviews plus a machine-readable generation report from an exclusively owned validated generated world. `--worldgen-preview --worldgen-output=<directory>` invokes it offline; row orientation is +Y-up and 4x4 fine samples are averaged per pixel. It shares contour/fill-height clipping and authored material/biome colors. This is separate from GPU texture/contour acceptance.

The seed-only generator and profile100 mode are retired. Sessions use authored
`prepareWorld` and one-actor `initPrepared`; WorldJob requires an explicit preparation
callback. `Colony.AuthoredFixture.Tests.h` is private test support, registered only
with game/app tests. Its typed descriptor pack uses the real importer/compiler/
generator and has no moving/gas reservoir, preserving zero-particle runtime tests.
Focused ECS/body fixtures can populate additional component actors without changing
production initialization. Numeric recipe generation tests remain in engine.tests.sim.
Smoke labels CPU adoption/component checks separately from its offscreen raster proof;
resident numerical continuation is covered by the GPU checkpoint/runtime gates.

`WorldGen.Capture.cpp` belongs to `game.colony.worldgen` and provides the explicit
frozen GPU diagnostic host; `game.colony.smoke` remains the separate smoke path.
The distinct `--worldgen-preview` selector requires explicit CPU/GPU output;
the older validate-plus-CPU-output form remains compatible. See
[authoring and diagnostics](../../docs/worldgen-content.md) for current commands,
checkpoint semantics and retained/scratch memory accounting limits.

Persistent matter is owned by Colony as `sim::MatterParticleState`. Generation spans
seed records once; checkpoint restoration uses saved records directly. Driver edit
candidates operate on the same authored catalog while detached for CPU inspection.
GPU-attached edits use the transactional emission/coverage policy above. Driver retains full current GPU fields/profile, durable epoch,
stable physical records and gas while detached for worker capture. v5 capture
never substitutes historical worldgen seeds for current state.
Frame packets share translated changed particle chunks and complete material/normal
and four-layer backdrop appearance; engine.app does not import simulation types.

Authored geology compiles one open-surface/bedrock frame, ordered warped bed
packages, material-only salt, clustered vacuum seeds and bounded pore/material/
finishing phases into the numeric plan. Supported roof/floor formations discover
bounded records from the frozen terrain before applying them; final certification
preserves the selected routed cave backbone. Every new control enters canonical
identity. The original weighted geological competition and weak solid-only finish
remain intact. A separate final `erosion_passes` stage uses the prototype's
unweighted eight-neighbor histogram including vacuum at 50% adoption, initially
eight passes in shipped recipes. Generator version 8 conservatively rejects older identities at
`validateWorldCheckpoint` before material-ID restoration, while retaining
the mandatory v5 checkpoint envelope. See [worldgen authoring](../../docs/worldgen-content.md).

The presentation cache also captures the immutable open-surface profile once
per world generation, evaluating the shared generation height helper at every
fine-column centre. Owned packets, initial batches and candidate publication
share those numeric heights with GridPass without transporting sim plan types.

Frozen worldgen capture and smoke prepare grid cells through the same bounded
`tryPrepareCellUploads` cohorts as interactive rendering. These explicit diagnostic
boundaries may wait for GPU progress, then refresh completed fences before retry;
they preserve captured content and do not add waits to ordinary game frames.

Temporary full-frontier aggregation diagnosis lives in Checkpoint.Gpu.Tests.cpp
(ull_frontier_frozen_fused_budget_pair), explicitly excluded with the existing
checkpoint GPU group from ordinary game.tests. Its exact diagnostic registration
owns the existing GPU lock and 180-second ceiling, one loop and at most 512 pairs.
The actual Driver/Box2D owner uses the frozen baseline solver; one normal candidate
GpuSolver runs strictly after each completed baseline tick and demand snapshot.
IMatterSubmissionObserverForTest borrows the finalized profile/source and all
eight per-body slices synchronously before Runtime transfer; the normal null path
copies nothing. The fixture detaches this borrowed observer before terminal drain.
Frozen source/module identities and first exact 22-field mismatch/rejection are
retained under fused-paired-1. This is a temporary diagnostic reference, not a
production backend; removal/retention is decided after the concrete result.

The temporary live-input diagnostic reuses the finalized submission observer.
Under --worldgen-report only, the app owns MatterSubmissionJournal and attaches
it before the actual Driver admission boundary; ordinary null observers allocate
and copy nothing. Its default-noop admission callback hashes actual semantic
rows/profile and the existing terrain masks once; repeated mismatched admission
invalidates evidence. A preallocated512-attempt, one-body CPU journal records exact
eight-slice inputs without callback I/O/allocation or GPU work. Driver shutdown
detaches first; the application retains CPU evidence through orderly shutdown and
writes it under its existing reporting path, preserving prior errors. The paired
fixture composes the same journal. PPRI/1 layout, hashes, bounds and conditional
comparison protocol are documented in fused-live-inputs-1/handoff.md. This is a
temporary diagnostic sidecar, not a checkpoint or numerical backend.
