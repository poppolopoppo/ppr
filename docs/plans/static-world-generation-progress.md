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
Independent Astra source/readability review passed. Focused app suites are still
running three loops in `out/worldgen-textures-app2.log`; their outcome will be
recorded before the next integration commit. Normal authored startup, candidate
request/ack integration, diagnostic modes, edit/collider coherence and stages
6–7 remain required. Metal's pinned texture creation waits synchronously and
has not passed the responsiveness gate; no backend-portability or total-memory
claim is made from these tests.
