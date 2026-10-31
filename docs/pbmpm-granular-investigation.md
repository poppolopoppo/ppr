# PBMPM granular investigation closeout — 2026-10-31

**Closed at the user's request. The visible pile problem was not fixed.**
The experimental physics and geometry-transfer prototype have been removed from
the active build. This record preserves findings, not a plan or authorization to
resume experiments.

## Restored baseline

The baseline is the source captured immediately before the granular investigation
in `out/pbmpm-granular-20261031/before/source/`. It includes the earlier validated
Sand objective-volume/terrain-friction adaptation and the digging crash fix. It
is **not bare Git HEAD**, which lacks some of that local work. “Stable” here means
this previously validated operational baseline; the reported clumping, porous
piles and premature grid coupling remain known limitations.

Restoration returns fresh solids to one sample per authored cell and the original
initialization, constitutive projection, profile admission and mean-volume
feedback. Four-sample factories, zero-history initialization, the new return-map
hook, positive-nodal-excess feedback and geometry-transfer test registrations
were rolled back together. Locally passing mathematical corrections were not
shipped independently after their combined pile behavior failed acceptance.

The current solid/liquid renderer, texture-interface filtering, rounded-contact
crash correction, and unrelated world-generation/UI work are preserved. All
pre-existing shader hashes in the preservation check match the investigation's
entry snapshot. Files outside the explicit restoration set are checked against
their pre-cleanup hashes. Existing baseline crash/contact, density/restart,
checkpoint and rendering tests remain registered.

## Findings

- **Premature grid coupling was reproduced.** Two blocks with a nominal 0.417 m
  gap exchanged about 5.2% of their momentum in one coarse-grid substep before
  touching. Their final geometric gap remained above 0.405 m. A fine-grid control
  had disjoint stencils and only quantization-sized changes. This establishes a
  mechanism, not a complete explanation of every late pile void.
- **The dilation-history probe exposed a defect.** An unloaded stored history of
  0.2 halved repeatedly in the baseline. An experimental coherent projection
  preserved logarithmic volume and passed local inverse-strain/yield tests, but
  did not establish acceptable integrated pile behavior.
- **Averaging can hide nodal overload.** Underfilled nodes cancelled overloaded
  contributions before the baseline density test. Gathering positive nodal
  excess reduced overload, but the one-sample experiment spread into a thin
  sheet. Combining it with four samples still left unnatural structures.
- **Sampling changes did not pass visual acceptance.** Four and sixteen samples,
  fixed and per-cell rotations, and a physically matched refinement control
  changed the results without producing an acceptable pile. Refinement reduced
  the central gap but produced excessive spread.
- **The geometry-compatible continuum prototype was insufficient.** It kept
  locally disconnected envelope regions in separate PBMPM fields. Connected
  controls matched all 22 observed state fields exactly; separated-block motion
  loss fell from about 24% to about 0.06%. The real 77-cell/308-row dug input still
  produced curved strands and holes at two seconds, despite its central mass
  moving closer to the floor. It was not integrated into production.

The prototype deliberately rejoined connected material into one continuum. Its
connected sliding control lost about 51% of relative tangential motion, exactly
as the native transfer did. It was not a persistent-grain Coulomb contact model.
Distinguishing residual nonconvex transfer shortcuts from insufficient transverse
sampling/affine deformation remained unresolved when work stopped.

The candidate also had material solver cost: at equal physical inventory, the
instrumented compressed/terrain workload GPU medians were approximately
7.95/12.98 ms for the baseline versus 15.64/30.41 ms for the four-sample candidate.
These are specific measured workloads, not an engine-wide performance budget.
No production GPU geometry classifier or acceptable production cost was proven.

## Preserved evidence and reproducers

Local evidence remains under `out/pbmpm-granular-20261031/` (ignored build output,
not portable checked-in test data):

- Original snapshots, shader variants, raw 88-byte particle rows, images,
  exact-command JSON records, logs and first failures are retained.
- `rotation-authored-controls.png`, `refinement-authored-controls.png` and
  `geometry-two-second-centres.png` show the rejected controls.
- `precontact-blocks.log` and `geometry-stage2-smoke.log` contain the coupling and
  connected/disconnected transfer controls.
- `geometry-actual-two-seconds-r8.log` and
  `qa/candidate/replay-geometry-two-seconds-r8/` contain the actual-input prototype
  result and unchanged-renderer captures. A completed ten-second prototype or
  two-second resolution-16 acceptance run is **not** claimed.
- `cleanup/pre-cleanup-source/` and `cleanup/pre-cleanup.patch` preserve all local
  source state before cleanup, including the experimental helpers and tests.
- `cleanup/binaries/{clang-dev,clang-rel}/` preserves the latest compiled
  diagnostic executables, repository runtime libraries, hashes and `ldd` records.
  `geometry-stage2-source/` binds the compiled prototype source to those binaries.
  The later validation-only energy/subcell additions were not compiled; their
  source is retained without claiming executed evidence.
- `cleanup/repro-tools/` preserves the analysis and execution scripts. Historical
  wrappers point at the normal build directories: do not use them unchanged to
  claim a frozen experimental replay after rebuilding the baseline.

For a future **explicitly authorized** replay, use a preserved executable with
the shader working directory and environment from its matching JSON record,
the archived runtime libraries, and a new output directory. For example, the
archived Release executable's
`app/matter_gpu/geometry_compatible_actual_four_sample_pile_diagnostic` takes the
recorded `PPR_PBMPM_GEOMETRY_INPUT`, resolution and end-tick settings, with
`candidate/` as its shader working directory. Do not copy experimental files back
into the live source tree merely to inspect evidence.

Useful baseline opt-in diagnostics remain in the active tests:
`game/checkpoint_gpu/actual_dug_solids_long_settle_diagnostic`,
`game/checkpoint_gpu/actual_dug_solids_short_compression_diagnostic`, and
`app/matter_gpu/sand_reference_occupancy_compression_and_floor_controls_diagnostic`.
The rendering-only full-resolution benchmark also retains its optional sample
count control. None is enabled as part of cleanup validation.

## Cleanup verification

`out/pbmpm-granular-20261031/cleanup/restoration-manifest.json` identifies every
restored/removed path, its source/hash, protected shader hashes and unrelated
local-file preservation checks. Final build/test and staging results are recorded
beside it in `cleanup/validation.json`. No save/checkpoint files or Git index
entries are modified, and no experimental trajectory is run during cleanup.

Cleanup validation passed on Linux: Debug and Release game/app-test builds,
format/style gates, four checkpoint checks, four GPU physics/contact checks, two
dug-terrain runtime checks and two solid-rendering checks. Physics and dug-terrain
checks used GPU-assisted validation. Astra independently passed the source
cleanup review. All 26 entry shader hashes and 81 unrelated local-file hashes
match; six staged shader trees match current sources. Windows checks and a new
performance benchmark were not run.

Further physics work requires a new, explicitly bounded task. The failed pile
acceptance is not converted into a completion claim or an automatic follow-up.
