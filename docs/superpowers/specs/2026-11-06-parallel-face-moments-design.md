# Parallel rays for reflected-light face moments

Status: approved by user2026-11-06; implementation authorized. This extends the selected parallel-ray idea
to a new entry point; the earlier direct-ray design explicitly excluded bounce.

## Evidence and choice

The optimized4096²/4K combined Matter/one-bounce/temporal capture exits0 and
proves accepted temporal history, but warm face moments cost34.219/32.856ms,
and the first matching Matter result takes2911.341ms. This is a shipping-build
cost problem, distinct from earlier Debug/native-validation overhead. Current
`bakeWorldFaceMoments` assigns one face per thread and serially traces its outward
half of256 direction bins. About1722 faces expose only27 workgroups.

Recommended: parallelize rays within each face while retaining ordered summation.
Alternative: split the serial face batch across frames; this can reduce spikes
but adds publication latency to an already failing1s path. Alternative: rewrite
DDA/skip storage; broader semantic and resource risk, not the smallest next step.
No speedup or complete1s result is promised.

## Bounded implementation

Retain the current serial entry as a qualification control. Add one64-thread
workgroup per face; GroupID.x selects the admitted face ordinal and GroupIndex
selects phi=lane+64*k for k0..3. Preserve outward-bin test, point/normal, full-world
trace bounds, unchanged worldTraceRaw, sky/media/partial-Matter semantics, all
source/error handling and ray totals. Inward bins explicitly initialize shared
storage but issue no trace.

Reuse the existing256-element R/G/B/status shared arrays (4096bytes plus existing
32byte integer counters). Every lane reaches all barriers. Lane0 loops phi0..255,
skips the same inward bins, applies the original diffuse-lift/256 expression and
adds in original order; no floating-point wave/tree reduction or reassociation.
Trace results are stored before weighting, preserving the current expression.
Every lane contributes integer trace counters; only lane0 reports the face output
and aggregate status. No global ray scratch or uploaded ABI changes.

The pass owns the new program/root/pipeline and dispatches one group per face,
with existing firstRecord/range admission and device group-count bounds respected.
Preserve face batch quantum, quota256 in the bounce route, publication ordering,
lookup, recursive semantics (still one bounce), cancellation/fences/teardown,
GPU Auto policy and all unrelated direct/cascade/Matter paths. No new public knob.

## Verification and retention

Use the existing native shared GPU fixture with serial-versus-parallel face
moments and final coefficients (1e-5 tolerance), exact status/query/segment/skip/
row totals, all four normals, non-multiple face counts, black/nonblack sky,
emission/medium, opaque and partial-Matter cases. Verify entry reflection/group
size and uniform barriers. Run registered ordinary/GPUAV correctness environments,
affected builds/style and independent source/lifetime review.

Then compare the saved optimized integrated baseline against the candidate on
Frontier4096 seed42/4K, unchanged32-cell pan and fixed diagnostic demands.
Measure warm moments, frame-joined producer costs and first-Matter age separately;
record capture history, clean shutdown and clocks. Keep6ms normal/10ms designated
stress,1s freshness and existing quality/memory gates. Do not automatically change
quotas if freshness still fails. Retain only with correctness and material measured
benefit; otherwise restore this candidate. This does not finish recursive feedback,
scattering, gameplay parity, total rendering/VRAM or sustained demand qualification.
