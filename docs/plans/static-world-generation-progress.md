# Static world generation implementation progress

The implementation authority is [the implementation prompt](static-world-generation-implementation-prompt.md).
This checkpoint records completed increments and outstanding integration; it is
not a declaration that the seven-stage acceptance criteria have passed.

The [acceptance evidence](static-world-generation-acceptance.md) consolidates the
current runtime measurements and open gates without rewriting this chronology.

## Implemented increments

- Asynchronous new-game foundation: first loading-screen presentation gates
  admission, owned candidate preparation, nonblocking observation, cancellation,
  background retirement, paused gameplay with continuing UI, publication epochs.
- Complete owned content catalog: strict material/field/biome/feature/profile
  validation, bounded image loading, canonical content fingerprint, two staged
  profiles and twelve materials. Checked 3:1 metric helpers and shared contour
  cases are available.
- Numeric generation: owned plans, independent deterministic streams, indexed
  nearest-site layout, required biome assignment, bounded placement/worm/room
  geometry, reservation-aware rasterization, bounded chunk scheduling and drain.
- Initial matter: sealed components including saddle openings, disjoint spans,
  integer fill boundaries, contour-clipped volume/mass and remaining-volume gas.
- Start validation: actor-footprint clearance, continuous level support, resource
  component minima and exposed resources within authored distance.
- Owned complete numeric result and headless generation integration: final
  terrain reference/boundary checks, explicit-clock phase times,
  canonical terrain/output checksums and material counts.
- Factory-only prepared-world ownership binds the catalog, reproduction descriptor
  and numeric result. Colony can adopt this owner without regenerating terrain;
  the driver adopts this owner with one controlled actor and metre-scale colliders.
  Interactive publication uses the authored pack and a single controlled actor.

Allocation failure is fatal. Accepted work drains before borrowed inputs are
released, including when an allocation exception must be rethrown.

## Closeout scope and remaining evidence

Stages 1–6 now have integrated implementations: authored asynchronous New Game,
transactional CPU/collider/GPU publication, catalog-driven textured terrain,
metre-scale control/tools, inspector/preview modes, full versioned checkpoints,
and CPU overview plus real GPU closeup exports. The entries below this section
are chronological evidence; earlier descriptions of pending integration are
historical and are superseded by later increments.

The user requested closeout of critical problems and errors after accepting the
current state. Independent review found no remaining concrete critical
correctness blocker; further performance tuning is deferred. This does not
convert unrun or unmet original acceptance gates into passes.

The eight profile/seed CPU and GPU capture cases passed;
32 overview images and 32 textured closeups decoded and were inspected. Known
generation scratch/payload accounting, the data-only material extension through
CPU/GPU presentation, cancellation/header retirement, distinct preview command
and explicit legacy fixture retirement are implemented and reviewed.

The bounded real runtime acceptance sequence and its resource observations passed
for both shipped profiles. Final Dev/Live/Release builds passed, and the current
twelve-entry Dev/ASan matrix passed in 959.87 seconds. Documentation and independent
review are reconciled for the requested critical-only closeout.
Native Inspector/extension readouts, progress-modal phase captures, both Cancel
paths and normal close passed the v2/v3 checks below. A ready-world four-frame
PIX GPU capture is saved with limited replay counters. Held-input traversal,
precise native input/Cancel latency, worker utilization, true queue depth and
precise GPU-memory measurements remain unverified.
Observed loading/live gaps remain a recorded limitation; final CPU frame
observations identify expensive initial terrain and actor draw calls without
establishing their internal cause. Process-memory
samples remain separate from generation-owned payload accounting.

Third-party dependency cleanup remains deferred outside the user's requested
critical-only closeout. The existing generated FastNoiseLite private-copy patch
remains a dependency follow-up. No further private library patch/fork is authorized.

## Evidence retained in the output tree

Earlier full ASan matrix and three-loop lifetime checks passed before the latest
numeric generation additions. Recent affected checks passed in
`out/worldgen-owned-tests.log` and `out/worldgen-matter-start-tests.log`.
The resource-distance regression passed in
`out/worldgen-start-exposure-test2.log`, including a positive control proving
that its negative case fails specifically on the distance guarantee.

Astra's read-only source reviews cleared matter, start and the complete numeric
result scope. The result and CLI builds passed; affected simulation and game
suites passed (73.17 s and 40.52 s) in `out/worldgen-result-tests.log`.
Both shipped-profile CTest checks exceeded the 300 s ASan timeout in
`out/worldgen-profile-tests.log`. An unrestricted Frontier seed 42 baseline
completed successfully in `out/worldgen-frontier-baseline.log`: layout 0.005151 s,
geometry 0.007015 s, raster 382.642785 s, matter 0.048924 s, final validation/hash
3.502982 s. Terrain checksum `6463df966ba72c4e`, output `23e8a7f54c511a28`;
64 sites, 17 features, 8 matter regions, 120 spans, 134217728 grid-payload bytes.
These are Debug ASan observations on the recorded Ryzen 9 3950X/64 GiB machine;
they do not establish release performance. The later Release corpus is recorded below.
The prepared-owner and nearest-ring increment passed the affected suites in
`out/worldgen-session-tests2.log` (sim 66.40 s, game 39.58 s), format/style checks
and Astra's read-only review. Build evidence is `out/worldgen-session-build3.log`.
MSVC 14.51 emitted an internal compiler error for imported inline lifecycle
instantiations; defining PreparedWorld/Colony lifecycle members out of line
resolved it while retaining ordinary RAII ownership.
The Release application build passed in `out/worldgen-release-build.log`.
All eight full-world corpus cases passed in `out/worldgen-corpus-rel/`: Frontier
and Hollows at seeds 0, 1, 42 and maximum u64. Observed elapsed times were
21.45–47.26 seconds; this single corpus is correctness evidence, not a paired
performance comparison. Frontier 42 retained terrain `6463df966ba72c4e` and
output `23e8a7f54c511a28`, matching the earlier ASan baseline.
The checked physics velocity/gravity API passed the affected ASan simulation
suite (67.16 s), including sleeping-body tiny-velocity, stale-handle and gravity
transition checks (`out/worldgen-locomotion-tests.log`); committed as `9e4de13`.
Stable driver locomotion and restore lifecycle regressions passed affected app/game
suites (78.06 s / 43.31 s), formatting/style and independent review; committed as
`ad839b0`. Shared-contour physics now uses the common 16-case topology, explicit
metric transforms and protected bounds, diagonal dirty fanout and owner-interest
closure. Corner queries materialize all dependent owners. Fatal allocation
propagation retains acquisition rollback. The affected ASan simulation suite
passed in `out/worldgen-contour-tests2.log` (70.22 s); independent review cleared
the seam fix; committed as `86a89c0`. Direct fixed-tick control is committed as
`63ca7ee`: it bypasses planning only for the selected existing actor, preserves
autonomous path advancement/movement and clears held commands at lifecycle
boundaries. The initial full game run exposed a fixture alignment failure;
the corrected focused suite passed and the full game rerun passed (50.65 s) in
`out/worldgen-control-tests2.log`. The affected app simulation suite passed
(96.61 s). UI focus/capture wiring and generated-session integration remain.
Actor metric adapters passed the affected app/game suites (93.83 s / 52.79 s)
in `out/worldgen-actor-metric-tests.log` and independent review. They retain
metre-based speed/body size, convert path targets and ladder bounds, and use
the checked conversion before integer narrowing. Prepared driver adoption is
committed as `db00b7f`. Prepared driver adoption is committed as `8963c0c`.
The movement suite passed three shuffled loops, including activation and
retirement of collider owners as an actor traverses without edits
(`out/worldgen-prepared-focused2.log`, 37.14 s). The Release prepared-session
and GPU smoke passed in `out/worldgen-prepared-smoke2.log`: Frontier 42 retains
terrain `6463df966ba72c4e`, adopts the same payload allocation, starts exactly
one controlled actor and checks movement, command reset and teardown.
The initial Release build was interrupted during repeated vcpkg compiler-hash
detection. Reconfiguration reused the already installed dependencies with
`VCPKG_MANIFEST_INSTALL=OFF`; the successful build is recorded in
`out/worldgen-prepared-release-build2.log`. Restore manifest installation before
changing dependencies; this local cache setting does not change dependency policy.
Catalog-aware tool and construction checks passed the affected ASan app/game
suites in `out/worldgen-tools-tests.log` (90.92 s / 62.83 s), format/style gates
and Astra review. Tests cover nonlegacy IDs, protected demolition, particle/unknown
ID preflight, cross-chunk tiles, actor overlap, reservations, progressive budgets
and preservation of the original failing tick stage. Direct UI input wiring is
implemented at the application boundary: WASD held commands, opposing-key
cancellation, focus/UI/pause/new-game clearing and camera-rate retirement at
ownership handoff. The application build passed in `out/worldgen-input-build2.log`;
all 58 camera tests passed (13.74 s) in `out/worldgen-input-camera.log`, including
the real priority-routing consumed-release regression. Generated-session
interactive publication and actual traversal remain acceptance work.
Later full acceptance gates remain pending. IDE inspection
has previously timed out and remains unavailable rather than passed.

Metric UI picking, actor selection, paths and cell/chunk overlays now convert
fine cells to session metres. Construction drags snap to complete global 3-by-3
tiles, preserve the anchor while clamping, and reject the incomplete edge tile.
Prepared publication retires the old owner before fallible camera setup. The
application build passed in `out/worldgen-picking-build2.log`, the game suite
passed in `out/worldgen-picking-tests.log` (58.66 s), and independent Astra review
passed. Interactive prepared publication and the GridPass metric/render seam
remain pending with stage 5.

Stage 5 renderer preparation now runs an optional synchronous callback before
opening each render pass, on the same encoder and submission as its draws.
The application/app/asset build passed in `out/worldgen-prerender-build.log`.
The GPU regression passed in `out/worldgen-prerender-test.log` (3.98 s), checking
ordering, rejection without fence advancement, and retry. Format/style and
independent Astra review passed. This is the compute integration boundary;
GPU culling, catalog textures and transactional world GPU bundles remain pending.

GPU visibility is implemented and used by the editor render lane. Reset and
cull write bounded per-slot visible IDs and one indirect argument record, with
explicit barriers and the existing completion-fenced slot governance. Empty
frames reset the count. Direct/reference and GPU draws share authoritative
submission bounds. Preparation uploads all submitted pending chunks before
culling, including offscreen cells. The ASan application/app/asset build passed
in `out/worldgen-gpu-cull-build6.log`. GPU IDs/arguments, 1024-entry capacity,
edge/empty/offscreen cases, cached-geometry pixel parity and repeated teardown
passed three shuffled loops in `out/worldgen-gpu-cull-test4.log` (8.87/8.35/8.81 s).
All 26 frame-fence leaves passed in `out/worldgen-gpu-cull-fence-tests.log`
(53.44 s). Format/style and independent Astra review passed. This does not
complete stage 5: textures, shared rendered contours, metric/catalog transport,
transactional GPU bundles and versioned atlas updates remain. The current
atlas still uses global idle; its replacement is a required responsiveness gate.

Prepared presentation now transports owned numeric catalog visuals, content
identity and the explicit fine-cell metric. Sparse terrain IDs remain unchanged
in uploads, including vacuum zero, while legacy fixtures keep their sentinel.
CPU reference culling, GPU culling, vertex positions and panel counts use the
same metre conversion. Supported scales and texture periods are checked before
unsafe arithmetic; shutdown resets the legacy metric. A borrowed driver catalog
accessor avoids a reproducible MSVC imported-member signature ICE in packet
capture without exporting renderer types into world generation.
The affected ASan build passed in `out/worldgen-metric-transport-build9.log`;
app simulation/presentation and game suites passed in
`out/worldgen-metric-transport-tests.log` (93.92/58.58 s). Three shuffled GPU
loops passed in `out/worldgen-metric-transport-gpu.log`, including metre-scaled
visibility IDs/pixels, CPU parity and shutdown/reinitialization. Format/style and
independent Astra review passed. This transport increment does not complete the
catalog GPU palette/textures, shared contours, persistent resident table,
versioned atlas or transactional world publication requirements.

The GPU lane now retains a bounded resident chunk table and completion-safe
slot uploads. Clean camera changes reuse it; submission bounds/materials,
residency, cached uploads and resource replacement invalidate it. Reference
draws invalidate the shared slot they overwrite. Build passed in
`out/worldgen-resident-table-build.log`; the GPU regression passed three shuffled
loops in `out/worldgen-resident-table-gpu.log` (10.75/10.17/10.30 s), proving no
table rebuild/upload after slot warmup and correct moved-bounds pixels. Grid
pass and all 26 frame-fence leaves passed in
`out/worldgen-resident-table-tests.log` (frame-fence group 42.42 s). Format/style
and independent Astra review passed. This records work-count evidence, not a
measured frame-time improvement or GPU-overlap claim. Versioned atlas and the
remaining stage 5 resource/publication work are still pending.

Interactive atlas uploads now use 2048 completion-protected physical slots and
bounded eight-chunk batches, without a queue-idle callback. Complete cohorts
publish their mappings and resident table together, including when frame
admission rejects the next draw. Identical unacknowledged revisions retain
partial progress; spare-slot exhaustion defers without copying or waiting.
CPU-staging/reset acknowledgment is distinct from publication so loading retries
do not discard a partial cohort. Explicit reference draws retain synchronous
preparation, completing all pending chunks when a flush is needed.
The ASan application/app/asset build passed in
`out/worldgen-versioned-atlas-build3.log`. All 59 focused app leaves passed in
`out/worldgen-versioned-atlas-tests.log` (87.13 s), including 26 frame-fence and
20 packet leaves. Three final shuffled GPU loops passed in
`out/worldgen-versioned-atlas-gpu2.log` (13.70/14.11/14.30 s), covering retained
pixels, cohort publication, replay progress, full-slot exhaustion, fence reuse,
publication without render preparation, and mixed reference preparation.
Formatting, semantic style and independent Astra review passed. This completes
the pass-local atlas increment; gameplay/collider edit revision coherence,
shared rendered contours, textures and transactional GPU world bundles remain
required stage 5 work. No UI-cadence or total-memory acceptance claim is made.

Catalog terrain now evaluates the exact engine.math contour table on the GPU,
including disconnected saddle components, row-major material ties, neighbor
samples across four-chunk seams, protected world-edge samples, and boundary-only
antialiasing. Legacy fixtures retain square cells. Retained resident tables also
retain their metric, contour mode and boundary material during deferred cohorts;
all neighbor mappings remain pinned until their readers retire.
The final ASan build passed in `out/worldgen-contour-build6.log`. All 59 focused
app leaves passed in `out/worldgen-contour-tests.log`, explicit ABI checks passed
in `out/worldgen-contour-abi.log`, and GameConfig passed in
`out/worldgen-contour-game-tests.log` (59.04 s). Three final GPU contour loops
passed in `out/worldgen-contour-final-gpu.log` (8.18/7.81/7.40 s); the broader
visibility/cohort regression also passed three loops in
`out/worldgen-contour-gpu2.log`. Saddle captures were inspected at
`out/build/msvc-dev/lib/engine/tests/asset/temp_gate_fixtures/grid-contour-case-{5,10}.png`.
Formatting, semantic style and independent Astra review passed. Textures,
transactional candidate GPU bundles, diagnostic modes and gameplay/collider
edit coherence remain stage 5 work; this increment does not claim those gates.

GridPass now owns terrain/atlas/resident state as one world resource value and
supports independent owner-lane candidate staging, bounded preparation, matching
activation and cancellation. Active terrain remains drawable until explicit
activation. Admission permits active+candidate or active+retiring only. Retirement
waits for the old reader fence and deferred pipelines, then drops completed
shared shader roots that also own atlas references; unfinished active roots are
preserved. Shutdown/loss reset fence-epoch state.
The ASan application/app/asset build passed in `out/worldgen-bundle-build6.log`.
All 59 focused app leaves passed three loops in `out/worldgen-bundle-tests.log`
(112.75/110.93/109.18 s, before the final candidate-only retirement correction).
Visibility and contour regressions passed three loops in
`out/worldgen-bundle-regression.log`. The final candidate regression passed three
loops in `out/worldgen-bundle-final-gpu.log` (18.36/19.71/19.29 s), covering retained
pixels, partial readiness, identity rejection, cancellation, three warmed shader
roots, atlas count 1/2/1, teardown and a fresh fence epoch after shutdown/loss.
Formatting/style and independent Astra review passed. Normal game request/ack
integration, catalog textures, diagnostic modes and edit/collider coherence are
still pending; the candidate API alone does not complete world publication.

Catalog surface rendering now reuses BindlessTextureCache with one shared wrap
sampler, per-world descriptors, a bounded 16-byte dense visual palette and
worker-decoded owned images. Sparse material IDs remain terrain identity;
source texture indices resolve to resident slots. World-space UVs preserve
periods across four-chunk seams. Tint colors decode from sRGB once; native
texture format controls texture decoding. Explicit gradients are computed before
coverage discard and bounded before period division. Stage validation admits
only tight single-mip RGBA images up to 2048 square, 16 MiB per image and
128 MiB aggregate. Interactive preparation admits one image per call; measured
UI cadence is still an acceptance gate.

Coordinator capture converts all resident cells directly to owned u16 uploads,
without a full sim::Cell snapshot, preserves pending revision manifests, and
observes cancellation between chunks. The normal WorldJob/render-control
handshake still needs to consume this product. Canceled prepared GPU bundles
now remain retained until a later accepted graphics-queue frame fence covers
asynchronous texture initialization; new admission stays blocked until retirement.
Both shutdown and device loss reset the upload/fence epoch.

ASan application/app/asset build passed in `out/worldgen-textures-build6.log`.
The textured sparse-ID/four-chunk UV readback, shared contour and candidate
retirement tests passed three shuffled GPU loops in
`out/worldgen-textures-gpu2.log` (47.09/46.50/47.01 s).
Independent Astra source/readability review passed. All 60 focused app leaves
passed three loops in `out/worldgen-textures-app2.log` (124.21/116.33/121.76 s). Normal authored startup, candidate
request/ack integration, diagnostic modes, edit/collider coherence and stages
6–7 remain required. Metal's pinned texture creation waits synchronously and
has not passed the responsiveness gate; no backend-portability or total-memory
claim is made from these tests.

Candidate preparation now has an independent owned render-control transport.
A validated immutable header is followed by batches of at most eight chunks;
readiness cannot seal a nonempty header before full coverage. Append/advance/
activation validate job, generation and fingerprint. The transport owns echoed
identity, retains acknowledgments without frame credits, cancels accepted work
during stop, and yields after each world batch so queued modal frames progress.
Application dispatches control results before ordinary frame accounting; Editor
rejects stale ordinary stages after activation. Allocation failure in the new
world executor is fatal; other exceptions become explicit candidate failures.

The final ASan build passed in `out/worldgen-controls-build2.log` (104 edges).
All 52 app control/fence leaves passed three shuffled loops in
`out/worldgen-controls-app2.log` (70.40/67.68/70.26 s). The focused GPU suites passed three shuffled loops in
`out/worldgen-controls-gpu2.log` (34.82/38.30/40.98 s). Format/style and independent Astra
review passed. This increment supplies the transport; normal authored startup
and the game-side publication handshake remain in the following integration.
The complete ten-entry matrix and backend/UI/memory gates remain stage 7 work.

Normal interactive startup now forwards the resolved root/profile/full-u64 seed
into one coordinator-owned authored prepareWorld factory. The 100-agent profiling
fixture remains explicit legacy generation. Physics and the complete owned u16
presentation capture finish off-main before candidate graphics admission. The
main lane moves at most eight chunk upload owners per admitted update, retaining
unadmitted commands for retry. GPU readiness precedes matching activation;
activation acknowledgment then publishes the CPU session and resets gameplay.
The previously paused session remains intact before that boundary. Close settles
an admitted activation before disposal, and Retry waits for the complete CPU/GPU
transaction. Candidate dirty revisions are acknowledged only after activation;
ordinary stale-generation frames cannot overwrite the newly active terrain.
Large presentation/candidate/old-session disposal remains coordinator work.

WorldJob retains bounded authored diagnostic text across cleanup; initial failure
renders the details and offers Retry/Quit, while retained-world failures log them
once before completion. The coordinator tests verify cancellation, committed
publication, graphics failure retirement, and retained file/field diagnostics.
The ASan application/game-test build passed 57 edges in
`out/worldgen-integration-build.log`; world-job suites passed three shuffled loops
in `out/worldgen-integration-tests.log` (6.04/5.18/4.33 s). Format Check, semantic
style and independent Astra review passed. Initial vcpkg compiler detection
stalled before compilation (`out/worldgen-integration-vcpkg-stalled.log`); configure
reused the installed dependency set with VCPKG_MANIFEST_INSTALL=OFF, with no
manifest/dependency edits. Restore dependency installation before a dependency
change. Full authored interactive publication/cadence still requires runtime
validation. Inspector/diagnostic modes, edit/collider coherence, checkpoint/full
capture/restore/exports and the stage 7 matrix remain outstanding; this increment
does not complete those requirements.

The ASan interactive missing-root run passed normal timed close (exit 0) and
reported the exact pack path/field in `out/worldgen-integration-invalid-runtime.log`.
This establishes responsive failure/cleanup control flow, not measured cadence
or visual inspection. A release authored-success run is queued separately.

The Inspector now resolves terrain names from the active catalog and shows the
sampled biome, authored feature footprints, and initial-matter material,
temperature, density and global fill height with an explicit unsimulated label.
Successful terrain changes invalidate the baseline preview; no-op digging keeps
it valid. Nearest-actor inspection measures metres. Generation details expose
pack/profile/seed, content identity, metric, phase times and metadata counts;
reload uses the existing confirmed asynchronous new-game transaction.

The ASan application/app/game build passed in `out/worldgen-inspector-build.log`.
All 21 sim-packet leaves passed three shuffled loops in
`out/worldgen-inspector-tests.log` (52.68/53.90/54.35 s), including no-op versus
actual edit invalidation. Format Check, touched semantic style and independent
Astra review passed. The earlier Release authored run exited cleanly after
150 seconds (`out/worldgen-integration-valid-runtime.log`), but its compiled
logging threshold filters publication messages; this is not proof of visual
publication or measured UI cadence. Diagnostic modes, coherent edit/collider
publication, checkpoint capture/restore/exports and final acceptance remain open.

The existing WorldJob coordinator now also accepts exclusive frozen-session
captures. Admission transfers the stable driver owner; queued, running and ready
operations remain pollable/cancellable, block another job, and return the owner
on success, recoverable failure or cancellation before worker-side payload
cleanup. A prepared cancellation consistently exposes operation_canceled.
Job kind is immutable so cleanup never races callback inspection. Interactive
freeze/ack deferral and diagnostic output are the next integration, not implied
by this foundation.

The additive sim::captureFull API copies every resident chunk in row-major order,
leaves dirty rectangles untouched, preserves ECS/body sections and checks
cancellation between chunks. It retains the v4 codec and ordinary dirty capture.
ASan build passed 88 edges in `out/worldgen-capture-build.log`; snapshot suites
passed three shuffled loops (139.2/133.9/129.5 ms) in
`out/worldgen-capture-sim-tests.log`, and world-job suites passed three loops
(4.38/4.77/5.39 s) in `out/worldgen-capture-job-tests.log`. Format/style and
independent Astra review passed after correcting a cleanup race, test
registration and ready-cancellation consistency. IDE inspection remains
unavailable from the earlier timed-out inspection attempt; no new pass claimed.

Diagnostic preview implementation is under validation. Material, biome,
temperature and initial-matter modes use the existing grid pass and owned world
candidate transaction, with a monotonic diagnostic revision. Biomes deliberately
cover nominal empty cells; temperature follows the solid contour winner; initial
matter clips nominal cells by fill height and the complementary empty contour.
A changed terrain baseline disables the initial-matter preview. Diagnostic
palette/index resources retire with their owning world bundle.

Preview preparation freezes the driver owner on the existing coordinator and
coalesces deferred render acknowledgments by chunk/revision until the owner is
returned. Frozen frames explicitly retain both grid and actor content while UI
and camera updates continue. Mode/range settings commit only on activation and
restore on failure/cancellation. The current implementation uploads a complete
replacement terrain bundle for a mode switch; its preparation and peak memory
costs still need acceptance measurements. It is not a palette-only switch.

MSVC 14.51 crashed internally when this sim-packet implementation called
PreparedWorld accessors directly. The coordinator now supplies the immutable
GeneratedWorld metadata explicitly; this avoids the compiler failure without
changing generation or metadata ownership. ASan application/app/game targets
build; focused GPU readback and lifecycle validation are in progress. No phase
completion or new acceptance pass is claimed yet.

Focused diagnostic GPU readback passed three shuffled loops (25.05/23.73/25.86 s
including fixture lifetime) in `out/worldgen-preview-gpu-tests.log`; world-job
coverage passed three loops in `out/worldgen-preview-job-tests.log`, including
preview failure and queued-cancel owner return. Independent Astra follow-up
review passes; format Check passes and semantic style has zero blocking findings.
The affected app run-gate/render-lane/grid/sim-packet queue is still running in
`out/worldgen-preview-app-tests.log`. Full matrix, runtime cadence/peak memory,
interactive visual checks, coherent edit/collider publication and stage 6/7
remain open. This is a validated preview increment, not stage 5 completion.

### Atomic edit foundation (2026-10-11)

`ChunkGrid::CapturedChunk` captures one chunk's cells, residency and exact dirty/activity state. Its allocation-free exchange supports preparing a candidate while retaining the old visible grid. Ordinary grid writes retain their existing residency behavior.

`ChunkColliders` can prepare bounded replacement batches under a Scene token. Physics stepping and query materialization are excluded until commit or cancellation; cancellation restores original chains, and destruction cancels before releasing the borrowed Scene. Tokens reject stale completion across Scene lifetimes. This foundation does not yet wire gameplay/GPU edit publication.

Validation: MSVC dev/ASan build of engine.tests.sim, engine.tests.app, game.tests and app.game passed (154 edges). sim/chunk_grid and sim/physics passed all 37 tests in each of three shuffled loops (190.5/185.7/216.9 ms); logs: out/worldgen-edit-foundation-build.log and out/worldgen-edit-foundation-tests.log. Format Check passed eight files; semantic style had zero blocking findings. Independent Astra review passed. IDE inspection remains unavailable as previously recorded. The preview app regression queue also passed 88 tests in each of three loops (353.67/361.03/355.01 s), out/worldgen-preview-app-tests.log. Full matrix and runtime acceptance remain pending.

### Interactive edit publication barrier (2026-10-11)

Interactive sessions now hold an accepted edit outside the visible grid. Dig preparation captures at most its four intersected chunks; build preparation captures only chunks actually written by the existing 64-cell tick budget. Candidate cells, build progress and presentation revisions remain owned until bounded collider preparation completes and a matching complete GPU-consumption manifest settles. Pathfinding, movement, physics and tick completion wait at that barrier. UI frames retain old content during collider preparation, then continue presenting while the GPU slot cohort fills. Initial-matter invalidation publishes with the whole cell cohort, including the synchronous reference path.

Already-admitted catch-up slices remain accounted for exactly once. Pausing holds them; one explicit Step completes one slice. Paused digs continue to work without ticking, and cannot incorrectly mark an unstarted tick prefix as executed. The in-flight dig owns its command independently of bounded FIFO overflow. New-game and preview requests defer during an edit, and restore/replay/reset APIs reject pending edits. Shutdown cancels the collider cohort before destroying its Scene. Independent review passes.

Validation: dev/ASan affected-target build passed, followed by three shuffled game/movement loops (12 tests including parent groups; 25.77/24.71/24.00 s), out/worldgen-edit-integration-v3-build.log and out/worldgen-edit-integration-v3-tests.log. Expanded diagnostic GPU readback passes three loops in out/worldgen-edit-gpu-v4-tests.log; asset rebuild is out/worldgen-edit-gpu-v4-build.log. Format Check passes eleven files and semantic style reports zero blocking findings. The first movement run exposed an unintended change to existing best-effort failure-path physics (shuffle 000000005E85EE8C); that behavior was restored and all three loops rerun. The first GPU run rejected an overly strict black-background assertion (00000000969DDAB6); it now uses the existing clear-color threshold, with the partial/full publication assertions unchanged. Original failure logs are retained. Full matrix, measured cadence/resource costs and interactive/backend acceptance remain open, so stage 5 is not declared fully accepted.


### Checkpoint envelope and frozen full capture (2026-10-11)

The version-1 game envelope retains exact reproduction/content/metric identity,
material manifest, authored metadata, full terrain/ECS/body state, completed tick,
controlled entity and baseline validity. The raw sim v4 codec is unchanged.
WorldJob captures through its existing exclusive frozen owner lease, and releases
untaken/cancelled payloads on the coordinator. Restore validates all identity and
bounded metadata before assembling a fresh unpublished session from saved cells;
it does not regenerate edited terrain. Transient plans/paths and held input are
removed. Derived colliders are rebuilt around actor interest, not for all 1024
chunks; Box2D solver caches are not persisted.

Validation: dev/ASan builds of app.game, game.tests and engine.tests.app pass
(out/worldgen-checkpoint-build-v3.log and -v4.log). Checkpoint/world-job/movement
coverage passes three shuffled loops (25 tests including groups;
40.36/38.14/39.96 s), out/worldgen-checkpoint-tests-v3.log. The cancellation-cleanup
delta passes checkpoint/world-job coverage in three loops in
out/worldgen-checkpoint-tests-v4.log. Format Check passes sixteen files; semantic
style has zero blocking findings. Astra independent review passes after fixing
vacuum report rejection, optional-instance omission bounds, span ordering and
coordinator cleanup. Earlier compile failures (missing offsetof macro, missing
direct test-module imports) remain in v1/v2 build logs; no runtime failure was
hidden. IDE inspection remains unavailable as previously recorded. Headless
preview exports, final authoring cleanup and acceptance gates remain open; this
is a validated stage-6 increment, not whole-plan completion.

### Flat diagnostic exports (2026-10-11)

`--validate-worldgen --worldgen-output=<directory>` exports four 1024-square
P6 images, a bounded JSON report and reproduction identity. Material, biome,
temperature and initial-matter previews average each four-by-four fine-cell
block; temperature excludes vacuum and matter uses shared contour-clipped empty
area. Biome colors come from the authored catalog. Output is an explicitly
blocking CPU-only diagnostic, with cancellation and I/O errors propagated.

Dev/ASan checkpoint/config coverage passed three loops (24 tests including
groups; out/worldgen-export-tests-v1.log); dev and release builds passed.
Independent Astra review and format Check passed; semantic checks have no
blocking findings. Release corpus passed both profiles at seeds 0, 1, 42 and
18446744073709551615. All eight reports sum to 16777216 terrain cells and all 32
images decode at 1024 square. Artifacts and exact commands are under
out/worldgen-acceptance; corpus-measurements.json records 28.35–52.37 seconds
per whole generation/export command, 158793728–159150080 bytes peak working set
and 182820864–183828480 bytes sampled peak private memory (100 ms sampling).
These CPU-only measurements include export I/O and do not establish interactive
cadence or GPU memory. Hardware is Ryzen 9 3950X, 64 GiB, RTX 2080, Windows 11.

The overview contact sheet shows differentiated large material/biome regions,
distinct denser Hollows caves and sparse localized initial matter. Fine cave
detail is visually busy at world-overview scale; closeup inspection remains
necessary before accepting terrain tuning. GPU closeups and final acceptance
are still open.

### Actual GPU closeups (2026-10-11)

The explicit `--worldgen-gpu-output` diagnostic renders a frozen authored driver
through the real bindless GridPass into an offscreen target, then saves four
512-square RGB readbacks and exact metre-camera/reproduction metadata. The
blocking upload/completion path belongs only to this UI-free diagnostic host.
It enforces GPU quiescence before resource release and honors explicit RHI
validation settings. Ordinary pack validation/flat export still initializes no
GPU. No simulation tick runs while these baseline images are captured.

The Release build passes (out/worldgen-capture-release-build-v4.log), and all
eight shipped-profile corpus cases pass in out/worldgen-gpu-acceptance. All 32
readbacks decode at the expected resolution. Inspection of closeups.png and
individual images shows readable flat start support, authored masonry repetition,
varied cave contours and no visible crack at the sampled seam. This does not
replace interactive actor traversal or all-seam automated coverage. Whole
generation/capture commands took 29.93–45.87 seconds; process peak working set
was 685330432–715431936 bytes and sampled private peak 821137408–855560192 bytes.
These are separate runs, not a paired export/capture performance comparison,
and do not measure dedicated GPU allocation or interactive active/candidate cost.

Astra independent review passes. Format Check passes six touched C++ files;
semantic style has zero blocking findings. Initial v1/v2 builds hit MSVC 14.51
internal errors for a PreparedWorld member, and v3 for its by-value signature;
the invocation boundary now passes the initialized driver plus camera/identity
values. Logs retain these compiler failures. The final dev/ASan build and
ten-entry matrix are running; stage-7 runtime/backend acceptance remains open.

## Preview selector and cancelled-header retirement acceptance

The full ten-entry ASan matrix passed against 9f577e6 in
`out/worldgen-final-matrix-v1.log` (912.23 s): core, sim, all six application
shards, asset and game. The next increment adds the distinct `--worldgen-preview`
selector, with an explicit CPU/GPU export directory, and makes plain
`--validate-worldgen` stop after loading and compiling the pack without GPU
initialization or terrain generation. The old CPU validation-plus-export form is
retained. GPU capture now requires the preview selector.

A cancelled candidate header may share catalog image storage with the UI-owned
command. WorldJob cleanup is withheld until that command is released, preserving
coordinator ownership of the final image release. A regression holds a borrowed
presentation across repeated cancellation polls and then permits retirement.

The new data-only material test adds ID 60000, its own texture path, metadata and
a new biome deposit. It checks generated ID transport, catalog metadata, digging
and low-level collider occupancy. It does not claim to exercise the Inspector UI
or GPU shading. Astra passed the retirement and corrected CLI/test scopes.
CLion inspection is unavailable: the execute_tool catalog request failed with
HTTP transport failure at 127.0.0.1:64362; no IDE-clean result is claimed.

The increment's 57-edge ASan build passed. All 44 selected tests (including group nodes) passed in three shuffled loops (44.18 s, 30.32 s, 34.93 s) in out/worldgen-acceptance-gaps-tests-v1.log; no assertion failures were reported. Format Check passed eight files and semantic style reported no blocking findings.

## Runtime observation and data-only GPU extension

Opt-in `--worldgen-report` now records update/control work, update and observed
presentation-ack gaps, first loading/world milestones and background outstanding
work. The clock is injected at the application boundary. Acks are CPU-observed
completions, not GPU timestamps; outstanding work includes running tasks. Offline
GPU manifests separately report CPU submission and host completion wait, plus
existing atlas counters. Astra passed this scope; configuration tests passed all
21 nodes in three loops, and dev/ASan and Release builds passed.

The actual data-only ID-60000 extension generated 4,827,397 terrain cells and
passed CPU export and all four real GPU captures. Images were decoded and
visually inspected in `out/worldgen-extension-acceptance/gpu/contact.png`.
Fingerprint: `3b8ab0a56ade1865`. The new deposit and authored feature material
require no C++ or shader changes. Inspector UI interaction remains unverified.

Release seed-42 runs used paced 60 Hz, paced vsync disabled and unfocused
throttling disabled. The first Frontier run observed loading in 2.485 s and
world readiness 9.202 s later, with a 155.054 ms terrain update gap. Repeats
observed Frontier loading in 3.181 s and readiness 52.602 s later, and Hollows
loading in 3.240 s and readiness 44.328 s later. Their layout update gaps were
469.165 / 392.166 ms despite maximum update work of 0.425 / 0.239 ms in that
phase. Presentation acks can be consumed in admission pumps between updates;
these values do not attribute the gap to generator work or establish responsive
input. The unexplained gap is an open acceptance concern, not a passed gate.

Repeat runs both exited zero. Process peak working sets were 679,419,904 /
667,521,024 bytes, and sampled private peaks were 892,022,784 / 879,976,448 bytes
(100 ms sampling; whole process, not precise generation-owned allocations).
Commands and memory: `out/worldgen-interactive-memory-v1.json`; reports:
`out/worldgen-interactive-frontier-repeat.stdout.log` and
`out/worldgen-interactive-hollows.stdout.log`. These startup runs do not measure
active-plus-candidate reload memory or native input latency. Format Check passed
five telemetry C++ files; semantic style has zero blocking findings.

## Bounded content diagnostics

Production job/CLI collectors and temporary content-schema collectors now retain
at most 64 records each. Failure counts survive truncation and nested forwarding;
formatted/UI/CLI diagnostics expose omissions. Defaults remain uncapped for
existing callers. A saturated total fails closed in schema, environment and CLI
validation until clear(), so a full collector cannot turn malformed input into
success. Bad allocation remains fatal.

Astra passed after correcting nested omission multiplicity and saturated-count
validation. The final 410-edge ASan build passed, followed by three shuffled
loops of all 64 selected core configuration nodes (2.01 s / 0.699 s / 1.80 s)
and 49 selected game nodes (9.78 s / 7.32 s / 7.51 s), including malformed
200-field content, exact nested omission counts and world-job lifetime paths.
Logs: `out/worldgen-diagnostics-{build,core-tests,game-tests}-v3.log`.
Format Check passed nine files; semantic style has zero blocking findings.
The final phase-7 full matrix and runtime acceptance remain outstanding.

## Explicit legacy fixture retirement

The production sim module no longer exports the hard-coded generator. Its
unchanged algorithm and six pinned-output/archetype/cancellation tests now live
in game-owned `WorldGen.Fixture` sources. Fixture callers explicitly select
`initLegacyFixture`, `regenerateLegacyFixture` or `WorldJob::legacyFixturePrepare`;
ordinary authored startup cannot silently fall back to it. The fixture's default
execution is now serial; explicit executors retain the drained parallel path.
Authored wall tools use the profile's boundary material and the UI says Wall.

Astra passed the independent read-only review. The 27-edge ASan build passed;
three shuffled loops passed all 34 selected game nodes (127.31 / 122.90 /
125.49 s) and 25 selected sim nodes (48.04 / 47.05 / 47.93 s). Packet tests
also run three loops in the same serialized queue. Logs:
`out/worldgen-fixture-{build,game-tests,sim-tests,app-tests}-v2.log`.
Format Check passed 18 fixture C++ files; semantic style has zero blocking
findings. The game-config CTest timeout is now 300 s to accommodate the six
full-world regressions moved from sim into game.tests.

## Known generation payload and admission observations

Generation reports now record retained numeric/container payload capacities and
five phase scratch observations. Raster scratch uses a conservative live-task
chunk-buffer bound after all accepted work drains. The UI and JSON explicitly
exclude allocator/reallocation overhead, opaque noise/task storage, stacks,
content, physics and GPU storage; these are not process peak measurements.
Checkpoint v1 omits build/process observations and restored reports label them
unrecorded. Checksums remain independent of timing and allocation observations.

Opt-in admission counters separate controls, pacing/clock and credit probes,
including failing/throwing phases, skip streaks and requested sleep overshoot.
The game also reports control-poll gaps and full main-lane render/build duration.
The shared injected clock remains explicit; disabled admission instrumentation
performs no observation reads. Existing exact-dt tests retain their clock.

Astra passed after fixing failed-phase timing and the uncapped manual-clock test.
The 270-edge all-target ASan build passed. Three shuffled loops passed all 45
selected sim nodes (50.08 / 50.90 / 52.69 s), four app nodes (65.05 / 66.95 /
63.15 s), and five checkpoint nodes (9.90 / 8.32 / 9.15 s). Coverage includes
owned payload boundaries, serial/parallel checksums and raster scratch bounds,
disabled/enabled scripted admission and checkpoint observation omission.
Logs: `out/worldgen-accounting-{build,sim-tests,app-tests,game-tests}-v1.log`.
Format Check passed 26 files; semantic style has zero blocking findings.
Release runtime measurement and the final full matrix remain pending.

## Fatal allocation boundaries and subprocess proof

Render-lane and editor packaging allocation failures now reach the fatal boundary;
main-thread failures rethrow through the run-loop drain guard. Ordinary exceptions
remain visible recoverable errors. Unknown exceptions no longer masquerade as an
allocation failure. Two armed subprocess cases prove that allocation exceptions
from lane initialization and frame execution cannot return recoverable results;
the execution case waits for entry before allowing shutdown cancellation.

Independent Astra review passed. The 16-edge MSVC dev/ASan build and current
Release game build passed. All 28 render-lane nodes passed three shuffled loops
(23.49, 9.44, 8.87 seconds), including both fatal subprocess modes, in
`out/worldgen-fatal-app-tests-v1.log`. Formatting Check passed four files and
touched semantic style reported no blocking findings. The final complete matrix
and current msvc-live gate remain outstanding.

## Production runtime sequence and held-edit UI fix

`--worldgen-acceptance` now runs a bounded explicit production-path sequence:
current-world presentation/warmup, stationary camera, pan/zoom, a two-cell seam
dig with successful presentation witness, biome/textured previews, two New Games,
and final atlas/texture retirement. It rejects legacy `--profile100`; early close
and timeout fail. Fixed-size owner-lane snapshots measure encoded draw/dispatch
commands and terrain/cache work without cross-thread pass reads. Quiet-camera
checks require actual GPU command encoding and zero atlas/table work. Final
texture entries/slots must match the settled initial baseline.

The first actual run exposed a production UI bug: collider preparation retained
the committed world but called a panel helper requiring a missing single-use sim
packet. The panel now accepts an explicit publication-summary span; pending edits
supply an empty unpublished summary and ordinary frames supply packet-owned rows.
This preserves live UI while collider preparation proceeds. Astra passed the
correction and complete sequence scope; the CLI regression covers the legacy
combination. Nine-edge dev/ASan and five-edge Release rebuilds passed. All 22
selected game-config nodes passed three loops (0.398 / 0.373 / 0.384 s).
Format Check passed nine files; semantic style has zero blocking findings.

Both Release/D3D12 shipped profiles, seed 42, passed the full sequence in
`out/worldgen-scenario-v2-{frontier,hollows}.stdout.log`. Stationary and pan
windows encoded 81–83 draws and twice as many dispatches, with zero terrain/table
uploads, writes, serializations or growth waits. The presented seam edit uploaded
65,536 bytes, one atlas publication and one resident-table build, settling in
141.580 / 156.418 ms. Each preview/New Game uploaded 33,554,432 bytes; every case
returned to one atlas and seven live texture entries/slots. Observed snapshot
peaks were two atlases and fourteen live texture entries. Whole-process peaks
including active+candidate and repeated replacement were 1,010,188,288 /
1,014,214,656 working-set bytes; 100 ms sampled private peaks were 1,225,273,344 /
1,232,449,536 bytes. Exact commands and process observations are in
`out/worldgen-scenario-memory-v2.json`.

These resource/command checks do not establish responsiveness. Frontier observed
205.286 ms maximum live update gap and 210.627 ms retiring presentation-ack gap;
Hollows observed 77.219 / 64.457 ms respectively. Control-poll gaps stayed near
32 ms against configured 60 Hz/16.667 ms pacing. Independent maxima and phase
boundary attribution do not prove the cause; these remain explicit acceptance
concerns. Native input/Cancel latency, worker utilization/true queue depth, actor
traversal, Inspector interaction and saved GPU profiler capture remain unverified.
No existing scheduler API separates running work from true queued tasks.

Fresh CPU seed-42 reports are in `out/worldgen-memory-acceptance/{frontier,hollows}-42`:
retained known payloads 134,451,392 / 134,572,880 bytes; maximum known phase
scratch 2,101,584 bytes in both. Raster scratch observations were 2,020,712 /
2,037,092 bytes. Whole-process CPU peaks were 159,088,640 / 159,354,880 working-set
and 183,623,680 / 183,898,112 sampled private bytes. Current checksums match the
retained corpus. Accounting excludes opaque allocations, allocator overhead,
reallocation overlap, stacks, content/physics/GPU storage and is not total memory.
The following final-matrix section records subsequent build/test results; manual gates remain pending.

## Final matrix and bounded retirement follow-up

The twelve-entry MSVC dev/ASan CTest matrix passed at runtime revision `427a2fc`
in 905.78 seconds (`out/worldgen-final-matrix-v2.log`). This includes both new
CPU-only shipped-profile validators. The serialized queue continues with the
focused retirement regressions. The preceding app/render-lane and asset/render
gates each passed three loops. The fresh MSVC Live build passed all six executable
targets in 221.77 seconds using MSVC 14.51.36231.

Independent source review identified whole retired-world destruction on the
render owner as an unsliced disposal operation. This is a concrete bound violation,
not evidence that it caused the recorded presentation gap. The follow-up retains
the fence/admission guards while releasing at most eight combined terrain and
diagnostic chunk blocks, one uploaded texture reference and one source image per
service step. Matching cancellation also retires never-uploaded CPU candidates.
Completed roots detach once per owner; cache epoch resets clear upload handles.
The final fixed number of GPU-object releases remains an opaque driver operation.

Behavioral GPU regressions cover bounded disposal with continued old-world draws
and textured shutdown/device-loss epoch replacement before old-world redraw.
Independent Astra review and Format Check passed; semantic style has no blocking
findings. The current dev build passed in 57.67 seconds; GPU regression execution
passed all three shuffled loops (51.25 / 55.42 / 55.17 seconds including fixture teardown).
Broader repeated app/asset gates continue in the serialized queue. Updated runtime measurements
remain pending; the preceding matrix tested the prior binaries.

All four terrain/visibility shader entry points compiled for DXIL and SPIR-V
(eight zero exits and nonempty outputs, `out/worldgen-shader-targets-v1/report.json`).
This is compiler evidence, separate from actual D3D12 execution; Vulkan runtime
readbacks remain unverified.

## Bounded-retirement runtime correction

The new retirement regressions passed three shuffled loops, followed by three
loops of `app/grid_pass,app/frame_fence` (39 nodes, 62.29 / 64.74 / 65.13 seconds)
and `asset/render/gates` (18 gates; 135.40 / 138.54 / 139.51 seconds including
fixture teardown). Dev, Live and Release builds passed for the affected targets.

The first updated runtime (`out/worldgen-scenario-v3-frontier.stdout.log`) failed
its settling assertion at seed 42: after the fixed two-second observation window,
retirement still retained two atlases and fourteen texture slots, with seven
reachable texture entries. This is not a passing resource-baseline check.
Hollows was not run after that failure. The acceptance command now treats the
window as a minimum, continues polling for the exact original atlas/texture
baseline, and retains the existing overall deadline and incomplete-close failure.
Independent Astra review and formatting/style checks passed; updated runs follow.

That failed run observed a 157.633 ms layout update gap and a 47.190 ms live gap;
maximum control-poll gap was 31.753 ms. It does not establish responsiveness or
prove the cause of earlier gaps. The sampling wrapper did not retain a valid
process exit code for this run; the explicit application error and failed
assertion establish failure. Subsequent wrappers pin the process handle before
sampling so exit status remains available.

## Native UI and Vulkan acceptance correction

The updated scripted v4 runs passed both profiles with actual zero process exits
and exact resource baselines after 3.111 / 3.122 seconds of bounded settling.
Elapsed sequences were 64.752 / 64.803 seconds. Latest measurements replace the
v2 table in the consolidated acceptance evidence; cadence remains an open gate.

Native UI captures under `out/worldgen-native-v1` show catalog names/temperatures,
the New Game confirmation, terrain composition and diagnostic preparation.
New Game reset the session; diagnostic Cancel retained it. This exposed three
defects: keyboard ownership reset cleared Inspector follow, focus used the view
corner and unreadable zoom, and Cancel was labelled as generation failure.
Normal close then exited 22: after the CPU driver moved to background disposal,
an empty stage reused the previous preview's generation and attempted to replace
the active catalog header. Close now retains the render bundle while draining.
Astra reviewed these fixes; native reruns remain required. The 495 ms Cancel
action/capture includes automation overhead and is not precise engine latency.

The test-only `PPR_ASSET_TEST_VULKAN=1` fixture asserts an actual Vulkan device.
Its first three-loop run exposed duplicate all-stage push constants from the
always-zero vertex tile-base parameter, missing CopySource target usage, and
tests using cached completion observations after a host wait. Removing the
unused scalar preserves all three terrain draw lanes; tests now declare copy
usage and poll retirement where they require completed fences. No third-party
source was changed. Astra review, formatting and style passed. Dev/Live/Release
affected builds passed in `out/worldgen-native-fixes-*-build-*.log`; all eight
offline shader targets passed again. The five selected Vulkan gates passed three
shuffled loops with no Vulkan validation errors (`out/worldgen-vulkan-gates-v2.log`,
`VULKAN_PASSED` in `out/worldgen-native-fixes-validation-v2.times.log`). D3D12 full
terrain-gate stress is running; final current matrix and native reruns follow.

## Native timed-wait experiment

The complete D3D12 terrain gate group passed all three shuffled loops on
`901b75b` (`out/worldgen-native-fixes-d3d-gates-v1.log`), finishing at
22:07:34 local time. The selected Vulkan gates had already passed three loops.

The v4 pacing observation included roughly 16 ms of requested-wait overshoot.
A focused Windows experiment replaces GLFW's coarse timed wait with an owned
high-resolution waitable timer and a simultaneous native-message wait. Posted
GLFW events still wake it; non-Windows implementations retain GLFW's timed wait.
The application still owns absolute deadlines and scheduling may overshoot.
A public-boundary smoke test covers polling, positive waits, posted wake events
and two service lifetimes without asserting scheduler timing.

Astra's initial review found rollback assuming registration after failed timer
creation, and platform shutdown discarding its retained error. Both corrections
are integrated and the focused re-review passed. Rollback now unregisters only
published services, attempts all owned cleanup and reports rollback failure
without replacing the initialization error. Normal shutdown propagates the
first retained error after independent cleanup. Formatting/style checks passed.
Native acquisition/close failure paths were reviewed, not fault-injected.
The serialized dev/Live/Release builds, repeated app checks and v5 profile
comparison are running; no pacing improvement is claimed before those results.

The native wait experiment built all six dev/Live executables and Release
app.game. An initial build rejected a Win32 DWORD logger field; explicitly
converting it to the engine's u32 field type corrected the diagnostic without
changing wait behavior. The broad scripted-platform repetition was deliberately
interrupted, not counted as a pass. The real GLFW player-service group passed
three shuffled loops (`out/worldgen-native-wait-app-tests-v2.log`).

Both v5 scripted profiles exited zero with acceptance PASS:
`out/worldgen-scenario-v5-{frontier,hollows}.stdout.log` and
`out/worldgen-scenario-memory-v5.json`. Elapsed sequences were 52.823 / 52.631 s;
live maximum update gaps were 18.918 / 19.644 ms versus v4's 32.998 / 47.859 ms.
These are single corresponding runs, not a repeated paired performance study.
Retiring-labelled presentation-ack gaps remain 67.607 / 68.166 ms; phase labels
attribute cross-transition intervals to their endpoint and do not establish
that destruction caused the delay. The first queued interval also reached
272.166 ms in Frontier. Acceptance remains open pending attribution/correction.

Native v2 used the existing ID-60000 extension pack. Captures under
`out/worldgen-native-v2` show persistent Inspector follow, Violet ore name/ID,
terrain composition, candidate graphics, and diagnostic cancellation retaining
the same session with a neutral notice. Normal close exited zero
(`out/worldgen-native-v2-result.json`), with a 935,321,600-byte peak working set
and 1,149,448,192-byte sampled private peak. Key taps did not establish walking;
body-contact correspondence and native traversal remain open. A New Game reset
succeeded; its attempted Cancel was blocked by the computer-use input guard and
is not counted as a cancellation pass. Diagnostic Cancel action/capture took
1,852 ms including automation transport, not precise engine input latency.

The user reported duplicate progress bars. Native preparation also displayed
"stage 8/7". A focused modal correction follows this validated increment.
Historical proposal supersession notes and the authoring-guide link were checked
and already exist in all three affected historical documents / docs index.

The progress modal now has one stage-local bar. Generation and graphics staging
show measured chunk counts; readiness, presentation and diagnostic preparation
use an explicitly labelled animated indeterminate bar. The invalid stage ordinal
was removed. Dev, Live and Release app.game builds passed, as did formatting,
semantic style and Astra's independent review. Native v3 captures show terrain
35/1024, graphics staging 952/1024 and indeterminate diagnostic preparation in
`out/worldgen-native-v3`. New Game published a replacement session; diagnostic
Cancel retained it with the neutral notice. The native run remains active for
the remaining acceptance checks; no traversal pass is claimed from key taps.

Native v3 also cancelled a New Game during terrain composition and retained the
previous seed/session. Normal close exited zero (`out/worldgen-native-v3-result.json`),
with 933,113,856-byte peak working set and 1,147,383,808-byte sampled private peak.
The apparent body overlap in the screenshot is the intentionally padded selection
outline; source review found no camera/metric transform divergence. This is not
a held-input traversal pass.

PIX UI launched the current Release game at Frontier seed 42 and captured the
ready, focused actor/support close-up. The saved four-frame capture is
`out/pix-manual/worldgen-ready-frontier-42-v1.wpix` (8,073,044 bytes), with PNG,
event CSV and reproducibility summary beside it. Its 428 API events contain two
compute dispatches and one indirect terrain draw per frame. Replay TOP-to-EOP
terrain-draw samples were 159,392 / 156,960 / 163,648 / 156,544 ns; these are
capture replay observations for one scene, not uninstrumented performance or
a paired comparison. The CLI rejected attachment to the GUI-launched process
with PIXTOOL17; GUI Take Capture succeeded. Replay analysis succeeded; its parser
reported zero errors and misclassified a progress line as one warning. The full
debug-layer stream was not available, so no blanket diagnostic-clean claim is made.
Timing captures remain unavailable through the unelevated server.

Focused gap telemetry retains the original maximum-gap metrics and records the
preceding update's phase/work plus the existing last-completed render-executor
gauge. It explicitly distinguishes sampled CPU execution/blocking from GPU time
and excludes pre-execution waiting/completion delivery. Astra review and format/
style checks passed. Corrected dev/Live/Release app.game builds passed in
80.70 seconds, and both isolated v6 profile sequences exited zero with acceptance
PASS (53.037 / 53.072 seconds). Peak working sets were 1,039,466,496 /
1,037,742,080 bytes. Repeated queued gaps of about 137 ms and cross-phase
retiring/live gaps of 51-69 ms remain unresolved. The low preceding update,
sampled executor and admission-probe durations do not establish a cause.
The twelve-entry CTest matrix passed all entries in 969.71 seconds on the
467cc13 binaries (`out/worldgen-final-matrix-v3.log`); PPR_TEST_LOOPS remains 1.
See the acceptance evidence for the v6 metrics and their limits.

A subsequent opt-in telemetry increment accumulates admission attempts and
skipped native waits/completion service until the next admitted update. It
records observed skip categories and sampled reservation occupancy beside each
maximum update gap. Format/style checks and Astra source review of the deterministic clock
regression passed. Dev/Live all-six and Release app.game builds passed in
174.87 seconds; both focused leaves passed all three shuffled loops in
156.33 seconds. Both isolated v7 profile runs exited zero with acceptance PASS
(51.772 / 53.031 seconds). The approximately 137 ms startup gaps contain seven
credit retries, while replacement gaps of 51.728 / 68.182 ms contain two/three.
Accumulated admission pacing and native busy waits account for those intervals;
busy completion service is below 0.011 ms. Retry pacing is the next focused
investigation. No new responsiveness
fix is claimed from this measurement alone.


The admitted-frame pacing correction preserves timer ticks and skipped-dt carry
while preventing denied attempts from re-arming the cap. Astra review and
format/style passed. Dev/Live all-six plus Release app.game builds passed
(178.37 seconds), and 52 selected frame/fence/admission nodes passed all three
shuffled loops (437.16 seconds process wall interval). Both isolated v8 profile
runs exited zero with acceptance PASS (53.405 / 53.992 seconds). The long gaps
persist: layout endpoints observed 137.506 / 124.887 ms, and replacement/live
endpoints 47.458 / 48.228 ms. At replacement, the sampled last frame executor was
64.069 / 65.085 ms. This directs further attribution to frame execution; it is
not yet evidence of a particular pipeline, resource or presentation cost.

## Critical-only implementation closeout

At the user's request, implementation work closes with no concrete critical
correctness blockers found by independent review. The admitted-frame pacing
correction is committed as `435dfbc`. Opt-in CPU frame observations now preserve
frame zero, carry frame identity/timestamps and separate staging/uploads/draw
callbacks from the encompassing surface duration. Their source review and
format/style gates passed.

Final Dev/Live six-target and Release game builds passed. Both corrected-binary
Frontier/Hollows v10 runtime scenarios exited zero with acceptance PASS and
returned to one atlas/seven texture entries/seven slots. The full twelve-entry
Dev/ASan matrix passed in 959.87 seconds (`out/worldgen-final-matrix-v4.log`), with
no failed assertion markers in its detailed log. The preceding 52-node affected
frame/fence/admission run passed three shuffled loops. `PPR_TEST_LOOPS=1` remains
set in Dev and Live caches.

The current acceptance record distinguishes implementation delivery from the
original performance/measurement gates that remain unmet or unverified. Further
tuning, held-input native traversal and the deferred dependency cleanup are not
represented as completed work. No additional feature work or private third-party
patch was introduced during this closeout.
