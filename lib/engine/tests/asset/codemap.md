# lib/engine/tests/asset/

## Responsibility

`engine.tests.asset` is the executable behind the `engine.tests.asset.*` CTest entries. It covers CPU image/mesh processing, resilience and logging, plus shared GPU render/cache tests and private-app quarantine/lifecycle tests. Current group registration lives in `Asset.Tests.cpp`; avoid copying leaf counts into navigation docs.

## Registration and fixture lifetime

`Asset.Tests.cppm` exports a declaration-only `extern const UnitTest asset`. Implementation units expose private `Tests()` accessors, assembled by `Asset.Tests.cpp`; `main.cpp` uses the shared engine test runner. Test assertions use `PPR_TEST_ASSERT`.

The root first runs CPU image, mesh, resilience and observe groups. The `asset/render` group acquires a shared GPU fixture for cache, gate, draw and shared-mesh groups, releases it, then runs quarantine tests using private applications. CPU-only filtered runs never acquire that shared device. Fixture scope is recreated per runner loop.

`CMakeLists.txt` registers image/mip, mesh/UV-normal, render/cache/gate/draw/shared-mesh, resilience/observe and GPU fixture implementations. Its always-run `engine.tests.asset.assets` prerequisite stages textures, optional meshes and shaders beside the executable independently of relinking, restoring missing files and pruning stale files. Test-only Mango/GLFW dependencies do not alter production module boundaries.

## Running

All entries use `--loop ${PPR_TEST_LOOPS}`, default 1. `engine.tests.asset.cpu` runs `asset` with `--skip-test asset/render` and TIMEOUT 450. Routine GPU correctness runs as five responsibility shards under `RESOURCE_LOCK gpu` and TIMEOUT 450: `gpu_caches` (the `caches` and `sharedmesh` groups), `gpu_picture` (picture controls, both HDR leaves, both MSAA4 spatial leaves, background magnification and both temporal motion-quality leaves), `gpu_grid_particles` (particle support/resources, candidate activation, compute visibility, stochastic world UVs/mips and surface inventory), `gpu_grid_surface` (the retained translucent boundary leaf), and `gpu_misc` (renderer/pipeline/MSAA/attachment contracts, GPU timestamps, pre-render ordering, plus the `draws` and `quarantine` groups). Each shard is an explicit leaf or group token list in `CMakeLists.txt` rather than a broad suffix match, so the union is auditable. Task 11 retired the smooth solid/liquid catalog-texture, large-offset, nominal-density, liquid-surface, liquid-optics, solid-envelope and solid-interface leaves with their shaders and entries; replacement material evidence lives in `app/world_lighting_material/`. Together these entries cover every remaining `asset/render` leaf exactly once; only the opt-in `*_gpu_validation` entries re-select leaves. ASan does not select three loops automatically. Configure `PPR_TEST_LOOPS=3` for stress and restore 1 afterward.

Run the routine entries with `ctest --test-dir out/build/<preset> -R '^engine[.]tests[.]asset[.]' --output-on-failure -LE gpu_validation`. The opt-in `gpu_validation`, `resident_particles_gpu`, `picture_hdr_gpu_validation` and `picture_spatial_gpu_validation` entries set `PPR_TEST_GPU_VALIDATION=1` and claim `RESOURCE_LOCK gpu` with TIMEOUT 300. They cover the representative grid visibility gate, resident-circle gate, both HDR policy/encoding gates and both indexed spatial resolve gates respectively; run them separately with the same target regex and `-L gpu_validation`. For focused groups, invoke the executable with the shared runner's `--run-test`/`--skip-test` filters matching current group paths; run from its staged executable directory. GPU groups require the documented platform/GPU environment.

See [root test policy](../../../../codemap.md#test-infrastructure) for machine exclusivity, required asset gates and loop selection.

For explicit Vulkan readbacks, set the test-only environment variable
`PPR_ASSET_TEST_VULKAN=1` in the serialized build shell, invoke the existing
`asset/render/gates` filters in a fresh process, then clear the variable. The
shared fixture initializes Vulkan before booting its headless application and
asserts the actual device type. The ordinary invocation keeps its existing default
backend. Invalid values and initialization/readback failures fail the tests;
the runner has no skip result, so unavailable Vulkan/bindless support must be
reported separately rather than counted as passing coverage. Private-application
quarantine tests do not use this shared-fixture override.

Grid reference/readback fixtures prepare bounded cell cohorts with
`tryPrepareCellUploads`, refreshing renderer fence completion before retries.
They retain content/pixel and candidate-retirement assertions; the removed
queue-idle convenience API is not a separate reference publication path.

The `picture_hdr_*` gate leaves draw controlled finite patch/ramp inputs through
`PostProcessPass`. Independent frozen double scalar values from the pinned
Filament approximation cover manual exposure, negative intermediates, saturated
HDR colours and FP16 output. Separate sRGB/UNorm readbacks exercise exactly one
encoding; rejected settings commit neither exposure nor output, and a later
native-resolution draw stays outside AgX. This fixture does not scan arbitrary
producer textures or qualify Hable TSR, ordinary-world lighting or the RTX 2080
frame budget.

The `spatial_msaa4_*` gate leaves first recover native sample indices using
rasterized threshold rectangles and per-index multisample loads. They exercise
the standalone `spatial_msaa4.slang` current-frame 2x resolve against a double
geometric grid oracle, independent constant/affine moment constraints, borders,
signed HDR and permuted indices. The threshold leaf additionally evaluates the
pixel-coordinate plane at every sample (`EvaluateAttributeAtSample`) and asserts
the exact raster-pixel top-left positions (3/8,1/8), (7/8,3/8), (1/8,5/8),
(5/8,7/8) and their index order, so a bracketed-only result is no longer the
sample-position evidence. Normal shader-directory staging includes the
new asset. The frozen MIT source and deliberate lower-bucket correction are
documented in `third_party/HableMSAA/README.md`. This is sampling/filter
qualification; runtime pattern admission, actual contour sample colours, temporal
history and 1080p-to-4K cost remain unqualified. Task 12 owns the reconstruction
integration; Task 2 removed the old runtime selector.

The threshold leaf additionally exercises the authored Slang-RHI MSAA capability
and scoped allocation-query recipe included by `SlangRHI.cmake`. It
checks consistent feature enum/count visibility, exact 4x sampled RGBA16F/D32
and single-sample storage queries, descriptor rejection before Vulkan queries,
unsupported extents and normalized mip rules. The recipe is now included;
its configure/include-path and execution gates must pass before claiming runtime
admission. See `third_party/SlangRHI/README.md` for the bounded descriptor scope.

## Routine GPU correctness shards

The former single `engine.tests.asset.gpu` aggregate selected all of
`asset/render` and could not finish its 450 s deadline: it accumulated ordinary
correctness leaves together with measurement loops, so the deadline was reached
before later correctness leaves ran at all. Routine correctness is now sharded
by responsibility (`gpu_caches`, `gpu_picture`, `gpu_grid_particles`,
`gpu_grid_surface`, `gpu_misc`), each with an explicit token list, the same GPU
lock and the same 450 s deadline. Nothing was moved into a longer deadline and
no leaf was dropped to make a shard fit.

The surface shard now covers only `grid_categorical_resolved_boundary_and_translucency`:
it publishes resolved cells and a uniform CH field, then asserts the opaque and
transparent categorical pixels and their boundary. Task 11 removed the retired
smooth solid/liquid reconstruction and its catalog-texture, large-offset,
nominal-density, liquid-surface, liquid-optics and separate
solid-envelope/interface leaves together with their shader families and the
`gpu_grid_liquid`/`solid_interface_gpu`/`solid_envelope_gpu` entries. The
replacement material evidence lives in `app/world_lighting_material/`; gas
visibility and blending remain covered by `gpu_grid_particles` and the resident
circle validation entry.

Pass lifetime is the dominant repeated cost: each `GridPass`/`PostProcessPass`
initialization compiles and creates its full program/pipeline family (about
12 s warm, 22 s for the first pass in a process), and the first draw on a pass
pays its pipeline creation (about 18 s). Leaves therefore keep one pass where a
control only changes the staged catalog: the catalog change travels through the
supported world-candidate replacement, and frames whose pixels are discarded use
`drawOpticalGridOnly_`, which keeps the real encode, wait and retirement but
skips the host readback.
`grid_categorical_resolved_boundary_and_translucency` and the retained picture
leaves follow that pattern.

Three leaves were pure timing/policy sampling with no unique correctness
contract and are retired here: `grid_material_shading_cost_samples` (four
material workloads × eight frames at 1024²), `picture_temporal_cost_samples`
(old temporal/palette combinations at 1024² and 256²) and
`grid_liquid_full_resolution_temporal_cost` (270 frames at 3445×1837 under old
Retro policies). Task 18 owns replacement performance evidence through the real
application profiler. Their surviving observation assertions remain elsewhere:
`m_background_targets == 3` and particle-instance counts are asserted by
`grid_particle_support_and_resource_repeat` and the resident circle validation
entry.

`temporal_layered_background_pan_uses_stable_spatial_fallback` and
`temporal_camera_pan_preserves_pattern_contrast_and_phase` encoded the contrast,
phase, MSE and disocclusion contracts of the removed temporal path and are
retired with it in Task 2. `picture_controls_preserve_independent_color_and_scene_history`
now covers exposure/bloom color independence, P1..8 pixelization blocks, stable
repeated captures and rejection without touching scene/history bytes.

The retired benchmark sampling also left `GridPass::setParticleTimingQueriesForTest`
and its `PPR_PARTICLE_*` environment hooks without a test caller. They are
test-support production code; their removal/adaptation is deferred to Task 18,
which owns profiling replacement, and is not part of this test-only correction.

`grid_liquid_optics_and_viewport_cost` and the retired solid/liquid appearance
leaves are removed with the categorical replacement. The surviving gas and
boundary leaves keep their background-change, roughness-off, instance-count and
pixel assertions; Task 11's `app/world_lighting_material/` suite owns the
replacement PBR, BRDF-table and transparent-composition evidence.

## Actual spatial integration qualification

The existing `render_accepts_msaa_sample_counts` leaf additionally renders the
production Triangle program with distinct payload groups into RGBA16F/D32F MSAA4,
then verifies resolved half-float colours and descriptor-backed per-group draw
constants. The retired spatial Retro lifecycle leaf rendered terrain with no
background; Task 12 owns the equivalent new-reconstruction coverage (recorded in
the execution ledger). Run the surviving MSAA leaf under explicit Core
validation and inspect the log for VUIDs; older RGBA8 or compute-only checks do
not qualify this path.

The GridPass per-index leaves cover centre-outside samples, mixed materials,
authored roundness, normals/emission and chunk seams. The retired
`spatial_retro_*` and `spatial_background_*` leaves and their four filters are
removed with the runtime selector; `spatial_msaa4_*` retains the kernel/sample
qualification. `spatial_hdr_grading_and_native_ui_follow_reconstruction` now
drives independent `PostProcessPass` grading at 512² and native UI after
presentation. Unsupported devices no longer exercise an opt-in runtime route.

The existing `spatial_grid_samples_preserve_contours_materials_normals_and_emission`
leaf owns one fixture-local `SpatialPrograms` family across its six original
opaque captures and the native optical/matter controls: two sample modules,
three sample programs and raster/dump/resolve pipelines are initialized once.
Each capture still creates fresh targets, roots and uniforms, uses the same Grid
candidate replacement and keeps every original pixel assertion. Other
`captureSpatial_` callers retain a private family through the original-signature
wrapper. Accepted or ambiguous submits retain the existing fatal-on-unknown
completion guard before per-capture resource release; the borrowed family
outlives that proof and the enclosing Grid teardown. No global cache, extra
application/device, policy/registration/deadline change or measured speedup is
introduced by this source refactor. The identical ordinary/GPUAV leaf remains
the measurement scope.

Each surviving leaf owns a disjoint ordinary/validation pair:
`spatial_grid_gpu` and `spatial_grading_gpu`, with matching `_validation`
entries. The `spatial_retro_gpu`, `spatial_background_gpu` and
`spatial_retro_gpu_validation`/`spatial_background_gpu_validation` filters and
their environment list entries are removed with the runtime selector. Every
surviving entry retains loops, GPU lock, native 300-second deadline and Linux
ASan unwind environment modifications; both validation entries explicitly set
`PPR_TEST_GPU_VALIDATION=1`.

This partition follows measured execution: the ordinary aggregate passed in
276 seconds, while Core+GPUAV reached its 300-second aggregate timeout after
three completed leaves. Task 2 removed the Retro and background pairs with the
runtime selector; the surviving grid/grading pairs keep their measurements and
deadlines. Individual case passes do not make the aggregate timeout a pass.
The fixtures provision six GridPass owners plus one small grading
`PostProcessPass` owner; raw material controls reuse one GridPass's
candidate/program lifecycle rather than initializing its full family for every
case.

The private `asset/render/quarantine/editor_scene_flow` reuses its existing Editor/RHI owner for opt-in final-surface readback after the resolved post output and detached native UI, checks a known UI pixel plus completion/owned-byte lifetime, suppresses repeated attempts, and checks rejected/no-image capture plus capability persistence through existing surface transactions. These source assertions require execution on a supported surface; they are not a new GPU fixture.

That Editor leaf additionally chooses an even non-64-multiple framebuffer width to require nonempty 256-byte copy-row padding, validates meaningful UI pixels and zero CPU padding/tail, and uses the existing delayed-retirement seam with real offscreen submissions to prove an armed capture survives busy credits before succeeding once. Stale surface rejection checks the explicit no-acquisition outcome before retrying with a current identity.

## Bounded bloom acceptance

`picture_hdr_punchy_matches_frozen_reference_and_single_encoding` and
`picture_hdr_rejected_policy_preserves_prepared_frame_and_history` reuse their
`PostProcessPass`/device/program fixture. Their constant/compact-patch controls
share a producer pipeline/root across captures. They cover off/disabled-again
byte identity, zero/subthreshold response, normalized constant response,
tiny/odd extents, finite 65504 at EV16, single encoding, native UI and rejection
without an exposure/history commit. The detailed ceil-level geometry, odd-edge
replication and fractional percentile mass are pinned in the app
`post_process` suite (`cpu_bloom_ceil_chain_layout`,
`percentile_trim_retains_fractional_mass`). The existing HDR GPU-validation
registration selects those same two leaves; there is no additional test
application, registered shard or CPU display renderer.
