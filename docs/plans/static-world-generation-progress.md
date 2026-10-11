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
three-cell construction validation. Interactive picking, input and presentation
integration remain in progress.

Stage 5 needs bindless textured contours, GPU visibility/indirect drawing,
incremental transactional GPU preparation/publication/retirement, diagnostic modes
and inspector integration.

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
