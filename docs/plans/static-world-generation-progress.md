# Static world generation implementation progress

The implementation authority is [the implementation prompt](static-world-generation-implementation-prompt.md).
This checkpoint records completed increments and outstanding integration; it is
not a declaration that the seven-stage acceptance criteria have passed.

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
  Interactive job/publication hookup is still pending.

Allocation failure is fatal. Accepted work drains before borrowed inputs are
released, including when an allocation exception must be rethrown.

## Outstanding plan work

Stage 3 still needs the catalog/session integration into the interactive job,
complete generation allocation/scratch accounting. The shipped seed corpus now
passes; visual inspection/export remains outstanding.
The current interactive candidate uses the legacy recipe.

Stage 4 has metric actors, shared-contour colliders, fixed-tick player commands
and preserved AI, prepared-driver adoption, catalog-aware dig/demolition and
three-cell construction validation, input clearing, metre-based picking and
camera/overlay transport. Normal authored-session publication and complete
shared-surface tool hit selection remain pending.

Stage 5 has GPU visibility/indirect drawing, persistent resident tables,
completion-protected atlas cohorts and shared GPU contour shading. It still
needs catalog textures, normal transactional candidate GPU publication,
diagnostic modes, inspector integration and gameplay/collider edit coherence.

Stage 6 needs checkpoint identity/full frozen capture, preview exports and final
authoring diagnostics. Stage 7 needs representative visual/traversal review,
performance/memory/responsiveness measurements, the complete required build/test
gates and removal of retired production paths.

Third-party dependency cleanup follows completion of all seven stages, per the
user's instruction. No further private library patch/fork is authorized.

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
