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
  interactive driver hookup is still pending.

Allocation failure is fatal. Accepted work drains before borrowed inputs are
released, including when an allocation exception must be rethrown.

## Outstanding plan work

Stage 3 still needs the catalog/session integration into the interactive job,
complete generation allocation/scratch accounting and the shipped seed corpus.
The current interactive candidate uses the legacy recipe.

Stage 4 needs metric migration of actors, colliders, picking/tools and construction,
shared contour collision, one directly controlled normal actor and preserved AI.

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
they do not establish release performance. Hollows and the full corpus remain pending.
The prepared-owner and nearest-ring increment passed the affected suites in
`out/worldgen-session-tests2.log` (sim 66.40 s, game 39.58 s), format/style checks
and Astra's read-only review. Build evidence is `out/worldgen-session-build3.log`.
MSVC 14.51 emitted an internal compiler error for imported inline lifecycle
instantiations; defining PreparedWorld/Colony lifecycle members out of line
resolved it while retaining ordinary RAII ownership.
Later full acceptance gates remain pending. IDE inspection
has previously timed out and remains unavailable rather than passed.
