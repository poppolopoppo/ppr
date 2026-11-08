# World-space lighting and post-processing implementation plan

## V22 reassessment, 2 November 2026

Newly available `docs/oni_v22.slang` justifies evaluating a different division of
responsibility: native simulation-cell world transport, directional refined world
probes, and rendering/screen-space treatment of fine visible geometry. The
[concrete simplification proposal](hrc-v22-simplification-proposal.md) records
active-code findings, the bounded border reproduction, resource estimates,
unmodified targets and a small playable experiment. It is a proposed revision,
not a v22 quality/performance pass. The current 69.45% aperture error is evidence
about the existing algorithm, not v22.

Further exact-contour implementation is held for this reassessment. The unbuilt
playable draft, minimal compile fix and old diagnostic paths are preserved under
`out/diagnostics/environment-lighting-20261102/receiver-scene/playable-phase2/reassessment-hold/`.
No broad rewrite or new GPU transport implementation was performed during the
reassessment.

## Current delivery priority, 2 November 2026

The full reference-driven environment, world-space RC and complete temporal
reconstruction/post-processing goal remains active and incomplete. Qualification
supports visible delivery; bounded diagnostic passes alone do not complete it.

The newly activated delivery goal prioritizes a representative playable scene
and visible results. A clean production capture now exists, but its sparse
composition and unlit backdrop do not meet the artistic reference. The user's
next explicit requirement is a lit cavern backwall that visibly receives door
and terrain shadows. Implement that receiving surface through the same RC field
first, then integrate the shared recipe into TurboLarbin's existing simulation,
camera, edit and render-worker paths. Do not create a second toy viewer. Temporal
reconstruction and bloom remain later stages; keep their full requirements.

The accepted background direction is optional C0 mean output in the same
immutable generation, followed by exact-view compute reconstruction with short
visibility connectors composed before interpolation. Both direct MSAA background
and cached backdrop consume a validated view, in linear space before exposure.
Collect aggregate status after its fence; reject unresolved candidates. Capture
may wait; interactive publication must retain the previous coherent view with
live UI rather than combine an old screen-space light texture with a new camera.
The finite receiving plane is the C0 convex hull; its outer half-cell ring is
declared far void, not an unlit wall or a query-failure fallback. No ambient term,
naive cross-wall bilinear replacement, or silent numerical fallback is accepted.

The corrected background implementation built successfully for `app.game`,
`engine.tests.app` and `engine.tests.asset`, with independent source review and
formatted author review passed. All 26 CPU lighting leaves passed. The first
physical background GPU leaf timed out at 300.49 seconds without an assertion
result or visible phase; it remains unqualified. Validation and compatibility
follow-ups were held. One independent production capture reached native
`cascadeTerrainTop` pipeline creation, then lost its monitoring session. Its
processes later disappeared without a retained application exit status or image.
That capture's outcome is unknown; it supplies no physical or rendering pass.
Actual translated/zoomed reconstruction and a dedicated high-resolution
shadow-edge displacement check remain outstanding.

The normal TurboLarbin playable handoff is approved for the first static-scene,
actor and camera checkpoint. Candidate preparation must preserve the last
completed world image beneath live native UI if active Grid replacement cannot
be atomic. Picking uses the displayed camera/revision; obsolete view requests
are coalesced. Native pipelines initialize during loading. Game-owned content,
Driver edit authority and coherent consumption acknowledgments remain required.
Door/lamp transactions follow the first checkpoint; unsupported edits must stay
explicitly disabled until their optical admission is implemented. Source-only
integration may proceed while the capture runs; its staged shaders, runtime pack
and frozen executable must remain unchanged.

The user's subsequent validation-time request takes immediate priority over
playable integration. Follow the existing asset/frame-fence shared-application
patterns to keep one live RadianceCascadePass across the strict receiver oracle
and background holdout, retaining distinct scenarios, unchanged assertions and
both final post-shutdown lease checks. Ordinary and GPUAV remain separate; private
failure/quarantine and operator-qualified precision cases remain isolated. A
merged filter without a shared pass would still recreate native pipelines.
Expose existing preparation logs and separate cold setup from warm scenario
results. Use a bounded durable runner that retains actual exit/timeout status.
The first cold creation cost is not removed by batching.

The three-file shared-fixture change now passes independent source review,
formatted author review and the app-test build. The ordinary strict batch uses
a verified transient user service with a 3000-second CTest bound
and a 3030-second outer bound. Success, failure and timeout/child cleanup were
verified with harmless runner probes. CTest explicitly selects GPU validation
off for ordinary and on for its separate validation entry. Both original named
scenarios and their numerical assertions remain; both lease families are checked
after successful final shutdown. Failed/incomplete scenarios retain leases through
quiescence proof and do not emit a lease-pass marker. The first ordinary run
ended with CTest exit 8 after 467.80 seconds. Its strict receiver oracle passed
in 375.74 seconds. Common top, merge and receiver pipelines were each created
once, taking about 7.44, 161.09 and 200.58 seconds at the host API boundary.
The background case reused that producer, then failed on Grid material-optics
admission of its fixture RGB. Its physical and final post-shutdown lease
assertions remain incomplete. The local fixture fix expresses the same S=(2,1,.5)
as legal RGB=(1,.5,.25) times strength 2, with exact optical projection asserted.
This demonstrates reuse and interpretable failure evidence, not a paired runtime
speedup or complete batch pass.

The corrected ordinary batch subsequently reached the physical gate in 61.35
seconds: receiver oracle passed in 32.03 seconds; constant/closed/open background
checks and the blocked-side limit passed, but physical brightness failed. One
isolated diagnostic localized the result: reference sum 0.27734375, rendered sum
0.18673718, relative L1 error 69.45%, signed bias -32.67%, blocked sum zero. CPU
and GPU C0 agree within 2.5e-10 per probe; GPU C0 and the rendered/albedo result
agree within 1.03e-5. This is a failure of the current RC sampling/merge
approximation on the fixed aperture, not demonstrated screen-mapping error.
The 15% gate stays unchanged. Approve only one CPU comparison: actual terminal
128-direction straight-ray control and a same-angle/same-radius candidate that
keeps near levels at the receiver position through the eight-cell boundary,
then reconnects to unchanged C4. Measure query/read counts. No GPU port or global
dense near-field storage is approved by that experiment.

That CPU-only comparison completed once in 7.16 seconds. Straight128 failed at
15.49296% relative L1; the same-q near-eight candidate worsened to 74.0537%
versus baseline 69.4542%. Closed leakage remained zero. Its 24 points required
14,976 connector/positive-interval queries and 12,288 coarse reads. Reject the
candidate; no GPU port, radius sweep or threshold change is approved. The
diagnostic registration is restored OFF. Resume the explicit experimental
playable showcase and a reliable lit/unlit capture to satisfy the user's visible
priority, while preserving this quality failure as an unmet acceptance gate.
This is not normal product activation or lighting qualification.

The subsequent durable headless capture completed successfully in about 1444
seconds with current binary `b6bc1bdc`, its current 38-file shader closure and an
isolated exact 649-file earlier asset pack. All eleven 3840x2160 PPMs are complete;
PNG conversions preserve their RGB bytes. Khronos Core validation was active at
instance/device and reported no VUID/validation error. This is not the old
`f7d183bb` shader snapshot or an interactive run. Lit/unlit pairs now exist under
`receiver-scene/production-environment-capture-playable/output/`, but the lit
backwall remains visually too dark and patchy for the reference direction.

Single observed exact-view GPU reconstruction times were 421--528 ms, with
165,888,944 view bytes, 10,368,000 completed samples, 445,500 far-void samples,
39,690,000 attempted and completed connector queries and zero unresolved/bound/invalid-value
counts per view. Desktop GPU contention was present. These observations are far
above the 4 ms lighting target; no steady-frame or 60 FPS qualification follows.
Native merge/receiver pipeline creation also took about 523/536 seconds. Keep
pipeline setup, world solve, view reconstruction and raster timing distinct.
The view's attempted/qualified fields count connector evaluations/completions;
they do not measure FP64 refinement attempts. Its current shader increments
global counters per connector, including identity fast paths.

### Actual experimental image evidence

The first closed/open game-scene pair succeeded at 3840x2160 through actual
RadiancePass -> GridPass -> 1080p MSAA4 reconstruction -> EV3/AgX, with Vulkan
Core validation active. All 434 closed and 420 open receiver records are valid.
The closed optical input bytes match the original captured dataset; the ten
previously failing receiver indices now succeed. This proves contributing query
refinement occurred, not that exactly ten queries retried. The unchanged receiver
oracle also passed (218.13 s). The first game capture took 475.48 s, including
cold pipeline creation, and is not a frame-performance measurement.

These experiments used isolated runtime copies with the frozen shared-query FP64
prototype overlaid. The independently reviewed production v2 implementation is
now applied to the shader and LightingPass sources, with all nine live file
hashes verified. The combined Dev C++ build and formatting checks pass, and the
nine staged shaders match their sources. The existing strict production receiver
oracle timed out at its registered 300-second deadline (300.60 s), without an
assertion or shader/compiler diagnostic; its phase and cause remain unknown.
Production public-admission checks passed ordinary/Core+GPUAV in 5.20/8.48 s.
The explicitly qualified production source observer passed its 36 rows and
helper-settlement counts 5/2/3/1 in 32.00/38.11 s ordinary/Core+GPUAV. Its linked
SPIR-V validates with Float64/RTE64 and the inspected precision controls. These
are source/helper results, not complete-query generation-counter qualification.
The separate complete refined receiver run timed out at 600.61 s. Initialization
and CPU C1 reference phases completed; preparation began but did not finish.
All four actual linked terrain modules exported and passed Vulkan 1.2 SPIR-V
validation with the inspected precision controls, but receiver results and
generation statistics were never reached. No retry or receiver GPUAV run was
made; qualification registration was restored OFF. Preparation-phase diagnosis
remains open, and this does not repair the strict 300-second timeout.
The new production capture completed; its first readback exposed a missing
CopySource texture usage flag. After the one-hunk target fix, the identical
five-image capture completed in 88 s with Core validation enabled and no VUID,
validation error or warning. Closed/open generations each had 30 qualified
refinement attempts out of 30; the environment had 42/42, with zero unresolved
queries or bound errors. Receiver record counts were 434/420/638 respectively.
All five old/new pixel rasters and the corresponding optical inputs, cascade
outputs and receiver records are byte-identical; the old run remains Core-invalid.
Validated 4K PNGs are in `receiver-scene/production-environment-capture-fixed/`.
They use nearest/flat response, not mapped normals. Single observed Grid terrain
GPU times were 4.95/4.91/5.81 ms, including terrain work rather than an isolated
lighting solve. They establish no full-world or 4K/60 qualification. The complete
independent production C1/Bezier comparison remains unmet.
Source/load-path, binary and input provenance for the earlier prototype images is retained under
`out/diagnostics/environment-lighting-20261102/receiver-scene/noinline-query-isolated/`.
All publication barriers remain intact; no invalid lighting output was rendered.

Pixel inspection identified a red source cast, an over-bright gray backdrop and
nearest-receiver lighting facets. The next palette-only capture changes the
scene-owned lamp to sRGB #FFCA75 with linear S=(2.5,1.4765471,0.44472104), and
multiplies decoded background tints by (.05,.11,.25). Shipped magma, geometry,
textures, camera and EV3 remain unchanged. This second closed/open pair passed
in 21.18 s; all receiver red values match the first pair bit-for-bit, and the
right-floor red response remains 0 closed / 0.00639953245409 open. The warm/cool
palette is clearer, while lighting facets and missing relief remain visible.
The comparable PNGs and exact-pixel PPM conversion records are in
`out/diagnostics/environment-lighting-20261102/receiver-scene/amber-navy-data/`.

### Next visible tranche and numerical policy

Implement an opt-in cut-face continuation experiment in the existing GridPass,
keeping nearest selection as the control. Transported receiver values,
self-emission, component boundaries and geometric coverage remain unchanged.
For squared distances s_i, m=min(s_i), R=sqrt(m)+0.5, use positive weights
`(m/s_i)^2 * ((R^2-s_i)/(R^2-m))^2` within s_i<R^2, then normalize. An exact
receiver centre returns its original value; conflicting coincident samples need
an explicit deterministic rule. Conservatively extend the existing CPU candidate
pruning for the declared FP32 support, without truncation.

This is not yet a leakage-safe production rule: same-component receivers may
lie on opposite sides of a wall. Test constant/anchor preservation, continuity,
one-cell partitions and narrow doorways using rendered linear lighting before
post-processing, plus shadow-edge movement. Preserve the <=1% blocked-side gate.
If side mixing fails, return a boundary-side neighbourhood decision; do not tune
the radius repeatedly against one scene or measure receiver values alone. Record
candidate counts/storage/preparation and actual GridPass GPU cost. Capture nearest
and interpolated variants from the same palette and completed lighting generation.
The first unrestricted experiment passed CPU candidate completeness (70.03 s)
and the GPU kernel check (3.11 s), but failed the independent physical gate:
16.15% mean blocked-side leakage versus the 1% limit, with zero for nearest
controls. Preserve this failure. The accepted next restriction anchors the blend
to the nearest receiver's outward midpoint-contour normal and includes only
same-component candidates with positive normal dot product. Retain the radius,
weights, masks, portal cases and thresholds; test the restricted candidate before
any visual or physical admission.
The restricted candidate passed the CPU suite (69.41 s), GPU kernel (3.22 s)
and unchanged physical gate (4.29 s). Both nearest and restricted interpolation
measured zero blocked-side mean/max leakage in all three fixed wall/portal masks.
The edge probes reported no crossing, so these runs do not measure shadow-edge
displacement or establish general physical or full-world quality. Frozen-build
provenance and raw results are under
`receiver-scene/combined-production-boundary/focused-tests/` in the diagnostic root.
Directional normal relief follows readable continuation; rough specular, bloom,
selected temporal reconstruction and richer environment composition remain needed.

For the next normal-relief tranche, retain the producer's existing 16 angular
directions with four spatial connectors each. These are 64 queries, not 64
distinct directions. Store the 16 spatially composed RGB bins separately
(16-byte records, 256 bytes/receiver), preserving the old 64-term mean order and
exact front-normal branch. Account for GPU/readback/CPU validation storage and
overlapping immutable generations before allocation. Reuse facing-continuation
weights, matched material normal-map sampling and camera-independent directions.
Keep a default-flat control and an explicit mapped-diffuse qualification mode.
With unit XY direction `d`, `A=dot(n.xy,d)` and `B=n.z>=0`, the normalized
elevation coefficient is `K=B+(pi/2)*A` for A>=0, and
`K=B+A*atan2(B,-A)` otherwise. Handle B=0 explicitly. In the negative-A
cancellation region `x=B/(-A)<=0.25`, use a bounded positive-factor
`x-atan(x)` series with an explicit remainder; avoid an unbounded B/(-A)
division elsewhere. Hardware qualification covers the actual shader atan.
K already includes twice the elevation integral: the normalized diffuse result
is `sum(K_i*L_i)/16`, not `(1/pi)*sum(K_i*L_i*delta_phi)`. Equivalently,
`E/pi=(1/(2*pi))*integral(K*L, phi)`. This preserves K=1 and the old angular mean
for the exact front normal without an extra factor of two.
The analytic hemisphere lift has an initial absolute coefficient-error target
of 1e-5 against independent elevation quadrature; finite 16-direction constant
fields target absolute error <=0.01 against continuous `(1+n.z)/2` for unit
radiance. These are local qualification targets, not replacements for the
accepted physical, resource or frame-performance gates. Measure the actual
shader helper, preserve separate self-emission, and do not add transport rays.
The bounded CPU-double calibration over 128 normals and the 16 uploaded
directions measured maximum coefficient error 1.094e-7 against independent
elevation quadrature, convergence delta 2.001e-7, and finite-direction constant
field error 0.003920. Exact front response was 1.0. These results support the
reference targets only; FP32/GPU precision, publication, rendering and cost are
unqualified. Artifacts are under `receiver-scene/normal-lift-reference/`.
The implemented mapped helper/lookup subsequently passed ordinary (4.40 s) and
Core+GPUAV (4.87 s) runs over 128 normals and 16 directions, including distinct
within-bin phases. Observed maximum coefficient error was 1.29514e-7 and
constant-field error 0.00644211, within the unchanged 1e-5/0.01 targets.
Flat/front bit preservation and weighted/facing lookup checks also passed.
This qualifies the observed helper/lookup corpus, not complete angular producer
publication, authored normal-map images, full-world storage or rendering cost.
Evidence is under `receiver-scene/mapped-diffuse/`.
The existing Grid/MSAA graphics compatibility leaf also passed ordinary
(89.51 s) and Core+GPUAV (158.76 s) runs with the new bindings. The rebuilt
kernel and unchanged physical masks passed again (3.20/4.45 s), with zero
blocked-side leakage; shadow-edge movement remains unmeasured. These test
durations include setup and do not establish frame cost. A separate verbose
600-second receiver diagnostic now exposes the existing preparation info logs
by temporarily lowering only the test logger threshold. It timed out at
600.72 s: top creation returned after approximately 428.4 s at the
createComputePipeline API boundary, then merge creation began and did not return
before the deadline. Dispatch, receiver results and generation statistics were
not reached. The ordinary run had no Khronos validation/GPUAV activation.
Qualification registration was restored OFF; the failure remains unmet.

### Bounded next artistic observation

The visible diagnostic has localized the wait after geometry, planning and
admission, at the first lazy `cascadeTerrainTop` pipeline creation. If it again
times out there without a compiler, module, device or numerical error, perform
one distinct, unchanged experimental Game capture with an 1,800-second wall-clock
watchdog on the verified device/session. This is not a raised CTest deadline:
retain the failed 300/600-second gates as unmet. The retained prototype measured
9.2 seconds of receiver code generation and 448.7 seconds of native creation;
three pipelines can precede the first actual dispatch.

Use the reviewed explicit qualification flag, immutable source/shader/scene
identities, existing status/fence/publication guards and per-generation real
retry statistics. Preserve completed pictures as they become available. The
current representative recipe is nearest/flat; do not describe it as a mapped
normal image. Inspect it against the references, then address its largest
visible defect and capture mapped response. A completed independent C1/Bezier
comparison, including angular bins when selected, remains required before
general producer admission. If this capture also exhausts its budget in an
identified native creation call, consider one artifact-local FP32 complete-query
noinline boundary experiment with retained call/precision controls, output
equivalence and separate code-generation/native timings. Broader Slang
optimization changes remain unselected.

The next visual deliverable is a separate game-owned 64x36 environment recipe
through the existing capture pipeline, preserving the two-room physical control.
Use thin pale/ochre banks, orange seams, dark ore pockets, a central shaft and
Grid-authored industrial platforms/pipes/tanks. Scene-owned opaque amber, violet
and cyan emitters may establish the composition with actual RC transport; they
do not qualify water, transparent crystal, refraction or participating media.
Keep shipped absorbing/blended material definitions unchanged. Existing mesh
props require RC lighting integration before their fixed local lighting can be
used in this scene. This deliverable must show visible progress against the
references, with matching exposure/framing and valid receiver generations.

The FP64 refinement policy is hardware-qualified to RTX 2080 / NVIDIA 615.78.08 /
Slang 2026.17.1 / Slang-RHI a78e208 and the recorded shader build. Its engineering
error envelopes are not universal Vulkan binary64 guarantees. The actual helper
corpus passed exact containment for 8,187 binary operations and 2,046 square roots;
seven tagged boundary/risk cases remain explicit arithmetic rejections. Actual
RTE64, NoContraction and absence of Int64/relaxed math were inspected; denormal
preservation is not supported. See `fp64-capability/arithmetic-corpus/` and the
[Vulkan precision rules](https://docs.vulkan.org/spec/latest/appendices/spirvenv.html#precision-and-operation-of-spir-v-instructions).

Product admission still requires qualification of the shared FP32/FP64 production integration,
one eligible complete-query retry with counters and unresolved failure handling,
consistent top/merge/receiver/physical-ray coverage, lifetime/resource accounting,
physical holdouts, full-world updates/media/bounces/gameplay integration and all
accepted memory, latency, 4K/60 and platform gates. The first images and their
palette improvement do not waive any of these requirements.

Status: accepted for staged implementation, 1 November 2026. Target hardware:
local NVIDIA RTX 2080 with 8 GiB VRAM; target presentation is 60 fps with VSync.
Output is 3840x2160 (4K), initially reconstructed from a 1920x1080 4x-MSAA
linear-HDR scene using the selected 2x-per-axis method. UI stays native.
Provisional user-approved full-world gates are 4 GiB peak lighting storage,
including overlapping generations, at most 4 ms of GPU lighting work per frame,
and at most one second of update age during normal-speed play. Initial physical
quality gates are at most 15% mean brightness error and 1% blocked-side leakage,
while retaining one-cell walls/doors and testing shadow boundaries before
post-processing. These are targets to qualify, not measured admission. The full
device capacity is not a lighting budget. The first implementation is
the CPU optical reference, immutable vacuum hierarchy/scalar DDA and allocation
estimator below. Bounded synthetic FP32 GPU trace/RC implementation equivalence
has passed source review and Vulkan core/GPU-assisted validation on this host.
The GPU vacuum hierarchy passed its bounded Vulkan execution gates; independent
RGB half storage passed its bounded Debug/Release gates. The authored material
optical bridge passed independent source review and focused Debug/Release gates. Production
world transport, physical RC quality and visual/display integration remain open.

## Intended result

Implement one camera-independent world-space transport solution with two
outputs: directional incoming radiance for rendering and delayed scalar
illumination for gameplay. All lighting originates
from emissive materials, including lamp buildings. Diffuse feedback supplies
indirect bounces. There is no separate light list or radial shadow-map system.

The target world is 4096 by 4096 fine cells, largely occupied by solids, with
destructible terrain, narrow tunnels and doors. Gameplay needs approximate
illumination for AI, growth and visibility; delayed updates are acceptable.
Probe positions and transport resolution stay fixed in world space through
camera motion and zoom. Display filtering handles changing pixel footprints.

Implement **accelerated world-space Radiance Cascades (RC)**. The historical v22
visibility/jump-table direction informs acceleration; its missing source is not
claimed as a verified port. The initial implementation uses an independently
authored aligned vacuum hierarchy. HRC is outside this
implementation scope: no HRC prototype, runtime backend or comparison project.
The choice follows the existing acceleration work and the predominantly opaque
world; it is an engineering recommendation, not measured proof of performance.
Use a slow ray-based numerical oracle to validate RC. That oracle is a test tool,
not a second production GI algorithm. The old ONI experiments inform the design
but do not establish correctness or PPR performance.

The algorithm reference is Osborne and Sannikov's
[Radiance Cascades paper](https://arxiv.org/html/2408.14425v1), particularly
§2.5 (bilinear fix) and §3.5 (sparsity). Record the reference revision when porting.

## Occlusion-preserving cascade merge

Make the paper's bilinear fix the baseline: trace a separate connector to each
of the four upper probes' child-ray starts, compose each connector with its
corresponding upper interval, then apply bilinear weights. Interpolating upper
lighting first is not equivalent. The paper reports four times as many traced
rays below the top cascade; this is not a claim of four times total frame cost.
It reduces parallax-related ringing/leaks, but does not repair undersampled
base probes or angular data. Budget and test the corrected merge from the start.

For each pair use `Lj = Lconnector,j + Tconnector,j * Lupper,j` and
`Tj = Tconnector,j * Tupper,j`, then `L = sum(wj * Lj)` and
`T = sum(wj * Tj)`. Apply weights once, with documented angular child mapping and
endpoint conventions. This explicit per-pair operation must survive packing,
compaction and sparse addressing. Padding/boundary probes need defined transport,
not clamped lookups that silently repeat a bright interior sample.
Interpolate transported `T`, not optical depth followed by an exponential.
Test a near absorber and far emitter to catch reversed composition order. Also
validate final C0-to-surface reconstruction: a correct cascade merge followed by
ordinary bilinear shading across a wall can still leak. Temporal display
accumulation cannot repair that systematic error.

## Fixed probe layout and bounded RC tuning

Use one C0 probe per fine cell at cell centers, anchored to a fixed world origin.
Start with eight equal angular bins and fixed phase. In fine-cell units:

```text
s_i = 2^i                 probe spacing
D_i = 8 * 2^i             angular bins
r_0 = 0
r_i = 2^(i-1), i >= 1     interval boundaries
cascade i = [r_i, r_(i+1)]
```

For 4096², fourteen cascades have a nominal outer radius of 8192 cells, beyond
the world diagonal. Clip rays to the defined world boundary. This count is not
yet a padded allocation specification: upper levels have fewer than two nominal
probes per dimension. Derive guard probes, their optical boundary values and
valid interpolation support before allocating. Do not stretch or clamp a coarse
grid at the edge to hide missing support.

This is the initial RC configuration, not proof of sufficient angular accuracy.
Use a finite tuning sequence against the optical oracle: eight then sixteen base
directions; if far-field sampling still fails, evaluate fourfold angular/range
growth with `D_i = D_0 * 4^i` and `r_i = r_1 * 4^(i-1)` while keeping
`s_i = 2^i`. Recompute cascade count, memory and tracing cost for that change.
Two-cell C0 spacing is a later optimization only after it passes one-cell door,
tunnel and emitter tests. Changing interval length independently of angular
sampling is a quality change, not a free optimization. Record one accepted
production configuration rather than accumulating runtime solver variants.

Zoom never selects another world probe spacing, phase, cascade length or angular
basis. Use world-footprint filtering only during visual reconstruction, preserve
occlusion boundaries, and qualify the smallest supported pixel footprint and
largest supported visible world area. Camera-driven cache residency must not
change the underlying field or full-map gameplay coverage.

## RC qualification gate

The v22 bitmask hierarchy and derived aligned-block jump table reduce RC ray
traversal work. Nearby opaque walls can terminate rays early; empty blocks can
be skipped. They do not remove RC's ray count, cascade storage/merges or angular
reconstruction errors. Transmission can extend traversal paths. Measure these
costs with the actual optical model and representative worlds.
Higher RC intervals can begin beyond the nearby wall that terminated a shorter
ray; do not assume every cascade benefits equally from short first-hit distance.

Complete this qualification before full gameplay and display integration:

1. Establish optical inputs, a slow transport oracle and resource
   estimators. Implement the independently useful accelerated DDA primitive.
2. Validate accelerated RC against the oracle on small-domain fixtures,
   first for emission/opaque visibility, then attenuation and diffuse feedback.
3. Estimate complete 4096² RC peak allocation, including all generations
   and the display chain. Do not allocate an inadmissible layout merely to
   discover the predicted out-of-memory failure.
4. Run admitted RC layouts on shipped generated worlds and stress scenes:
   enclosed rooms, one-cell tunnels/doors, diagonal walls, long corridors,
   excavated open chambers, distributed emitters and transmissive material runs.
   Measure initial solve, repeated edits and worst-case update age, not just
   warmed-up traversal. Collect actual allocated bytes and stage timings.
5. Compare RC against the oracle at matching world scale, transport reach and agreed error
   thresholds. Report shadow-edge error and leakage before display AA, and
   stability during zoom after reconstruction. More directions, tighter probe
   spacing and necessary refinement count in RC's cost.
6. Admit RC only when it meets the hard-shadow, transmission, memory and latency
   requirements. Record its layout, precision, hardware, errors and costs.

If RC fails, stop the full-map acceptance milestone and present the
measured tradeoff. Do not silently reduce transport range, erase narrow passages,
substitute blurred shadows or switch to another GI architecture. A change of
solver would require a separately agreed revision of this plan.
Approximate/delayed gameplay values do not waive visual occlusion correctness.
Hard shadows require small emitting features; finite-area emissive surfaces also
produce legitimate penumbrae that must not be sharpened away.

## Coordination with the material plan

The referenced chat **Overhaul biome generation pipeline** has produced the
parallel [world-generation and material plan](../worldgen-overhaul-plan.md).
It owns catalog/schema changes, authored textures, normal maps, metallic,
roughness, emission, opacity, IOR, four parallax layers and background-only
refraction. The overhaul now records implemented behavior and scoped validation.
Current `GridTerrainVisual` carries normal texture, metallic, roughness, opacity,
emission, IOR, normal scale and refraction thickness; `GridStagedFrame` owns
background layers and particle batches. Extend those actual interfaces rather
than creating another material catalog or backdrop system. Lighting transport
and the new display chain remain proposed here.

Agree on one versioned numeric lighting interface before either integration:

| Input | Contract |
|---|---|
| Albedo | Linear RGB after sRGB texture/tint decoding |
| Emission | Linear RGB factors times nonnegative HDR strength; emission enters transport once |
| Surface response | Metallic and roughness in [0,1], dielectric IOR |
| Transport | Explicit opaque, transparent or absorbing model; attenuation expressed per world metre |
| Coverage | Geometric contour coverage separate from visual optical opacity and transport extinction |
| Normals | Shared +world-X tangent, +world-Y bitangent, camera-facing normal convention; scale and stable world tiling |
| Identity | World, material, geometry and optical revisions sufficient for invalidation |

The transport model/extinction is an addition to the parallel plan's appearance
schema and must be agreed there. Do not infer light transmission from texture
alpha or antialiased contour coverage. Convert fine-cell distances to metres
at the optical boundary. Start with opaque blockers and transparent empty cells;
validate absorbing/colored transmission separately before enabling it in content.

Cheap refraction distorts the composed background only. It does not bend world
transport or inject parallax artwork into gameplay illumination. Initial scope
excludes refractive transport, caustics and volumetric multiple scattering.

Preserve the material plan's compositing rule. For covered-surface color
`S = o * materialColor + (1 - o) * refractedBackground`, composite
`c * S + (1 - c) * destination`, where `o` is optical opacity and `c` coverage.
Do not pass precomposed `S` through another blend weighted by `o * c`.
The shared backdrop already uses RGBA16Float storage. Preserve it, but audit
the entire composition path for linear-HDR semantics before tone mapping.
Current `grid_tiles.slang` uses fixed-direction facing shading and compresses
emission with `1 - exp(-emission * min(strength, 80))`; replace those display
approximations with the selected transport/PBR response and uncompressed linear
emission. Avoid applying the old compression before AgX.
Background ownership remains with the material plan's GridPass component.

The material overhaul now includes persistent matter. Build optical inputs from
the authoritative terrain and matter state with matching publication identities,
not the old initial-fill visualization or camera-visible particle batches alone.
Define particle/fill-to-transport coverage and density explicitly; visual radius,
alpha and compositing order do not establish a volumetric extinction model.

## What to take from oni_v22

[oni_v22.slang](../oni_v22.slang) contains compute preparation, BRDF integration,
explicit-light culling, RC passes, temporal directional reconstruction,
`shadeWorld`, an AgX Punchy transform and an output write. Its imported
`playground`/`rendering` host code is not available here. There is no verified
standalone bloom, exposure-histogram, motion-vector or TAA-resolve chain in this
file. Camera jitter alone is not TAA.

| Reference location | Planned treatment |
|---|---|
| 3564–3585, filtered procedural patterns | Keep footprint-aware filtering where the corresponding detail is used |
| 3762, AgX; 10058, Punchy output | Preserve intended look; validate numerical behavior and display encoding independently |
| 6688, circular-harmonic cache | Replace temporal/storage logic; signed coefficients, explicit validity and first-write initialization |
| 8426, LDS visibility-mask reduction | Optimization candidate after optical semantics, bounds and barriers are proved |
| 8558, aligned-block jump table | Reuse for accelerated DDA/RC after correctness checks |
| 9182, Hammersley jitter | Replace with the selected MSAA temporal reconstruction pattern in presentation only |
| 9374, BRDF integration LUT | Reuse concept after matching the selected BRDF and energy convention |
| 9382/9439, explicit lights | Exclude; emission supplies illumination |
| 9432, material response | Rebuild emission/diffuse/specular accounting around the selected transport solver |
| 9461, specular luminance clamp | Do not preserve a material-color ceiling on HDR lighting |
| 9579, RC entry points | Reimplement RC with validated intervals and oracle comparisons |
| 9923/9949, chamfer and cavity AO | Integrate with existing contours; avoid counting the same world occlusion twice |
| 9967, procedural normal detail | Optional artistic layer, separate from authored normals |
| 10065, output compute entry | Add proper dispatch bounds and PPR resource bindings |

Do not inherit unsigned packing for signed directional coefficients, history
blends that darken the first frame, or jitter-dependent reinterpretation of
old samples. V22's fourth-order circular harmonics need nine RGB coefficients.

## Reference solver, accelerated traversal and resource admission

First build small CPU references for interval composition, attenuation, angular
integration and diffuse feedback, with a slow ray-based comparison for small
scenes. Specify RC's angular layout/quadrature, probe locations,
cascade lengths, interval reconstruction rules and world boundary conditions.
Distinguish unavailable data from empty and opaque space.

The composition contract is `L = Lnear + Tnear * Lfar` and
`T = Tnear * Tfar`. RGB absorption requires RGB transmittance; one alpha channel
cannot represent it exactly. Start FP32 on small domains, then assess FP16 error.
For homogeneous absorption, use `T = exp(-sigma_t * distance)` in world metres.
Define surface emission separately from any volume-emission integral. An opaque
emissive surface contributes its source before terminating transmission. Derive
diffuse feedback normalization for the chosen planar angular measure instead of
blindly copying a 3D `1/pi` factor. Separate diffuse reflectance from metallic
response and keep the feedback energy bounded.

### Milestone 1 numeric boundary

`engine.app:renderer.lighting` supplies a small-domain, cell-constant CPU
reference. This is a reference fixture model, not the final marching-squares
terrain or reconstructed matter geometry. It introduces no material catalog or
simulation dependency. `OpticalKind` explicitly selects vacuum, opaque or
absorbing; unavailable input errors even if no ray would touch that cell.
All unused optical fields must be zero. Texture alpha, visual opacity and
antialiased contour coverage do not determine transmission.

Coordinates are world-anchored metres with an explicit origin and cell size.
Directions are normalized by the reference; finite near/far distances are metres.
Parallel-axis ownership is half-open `[minimum,maximum)`; corner-only touches
have zero length. Starting inside opaque material evaluates its source at the
interval start. Zero-length intervals are the identity. Exterior must explicitly
be vacuum or black opaque; unavailable boundary policy errors. A black opaque
exterior stops a ray before entry or after exit, while a vacuum exterior preserves
throughput. Neither policy models incoming environmental light.

RGB intervals compose near before far. Homogeneous source density `j` integrates
as `j * (1-exp(-sigma*d))/sigma`, using `expm1` for small optical depths, and
`j*d` at zero extinction. Opaque surface emission is added once before stopping.
This does not implement reflection, diffuse feedback or multiple scattering.
The reference intersects every cell analytically and sorts all positive-length
segments; it has no iteration cap or silently incomplete success. Invalid or
nonfinite inputs/results return errors. Every adjacent world-grid plane must remain
strictly ordered in the chosen double metric; large translations that collapse an
interior cell are rejected before tracing or hierarchy capture. This check uses
the same plane calculation as transport, not only the grid's two endpoints.
Allocation failure stays fatal at the application reporting boundary. This deliberately slow CPU path is a correctness
oracle only, with no production speed claim.

Remaining production work includes mapping the existing terrain/matter geometry
and authored materials to explicit extinction/source inputs and revision identity,
contour coverage shared by reference/RC, angular quadrature and diffuse-feedback
normalization. Those are required before claiming the material integration portion
of milestone 1 or the whole milestone is complete.

### Stage 3 qualification sequence

`RadianceCascadePass` first qualifies scalar FP32 GPU cell DDA against the CPU
optical oracle on synthetic bounded fields. It owns its shader, compute pipeline,
root and cell/ray/result/readback buffers, records through `PreRenderContext` and
leaves queue submission/fence ownership with the host. Admission counts candidate
plus previous live buffers and explicit reserve before GPU allocation. Completed
fences gate readback/reuse; encoded unsubmitted work needs explicit abandonment,
and ambiguous submission retains resources until proven idle or confirmed removal.
The owner now also evaluates the selected FP32 RC schedule on complete small
fixtures, using two fixed parity scratch buffers and readback of one explicit C0
tile plus the selected C1 probe0/bin0 diagnostic. It does not retain published C1,
allocate gameplay scalar/history resources or participate in game frames.

World-origin subtraction uses CPU double; uploaded origins are local fine cells,
with explicit metres/cell and source/extinction per metre. Unrepresentable/nonfinite
float values, collapsed positive intervals and unrepresentable integer grid planes
are rejected before allocation. The GPU result carries explicit failure status;
no incomplete interval is accepted. Scalar buffers use 40/24/32-byte cell/ray/result
strides with 32-byte constants, verified with layout assertions and active-backend
reflection. A small-depth polynomial evaluates the source integral stably where
`1-exp(-depth)` would lose precision; FP32 equivalence is tested against double.

For bounded fixtures the accepted numerical equivalence tolerance is
`1e-5 + 1e-4*abs(reference)` per L/T channel, with finite nonnegative radiance,
transmission in [0,1], and no shader failure status. Expected values consume exactly
the quantized upload field. This tests implementation equivalence, not physical
RC quality; absolute-world quantization and angular/geometry error remain separate.
Vulkan execution on this Linux host does not establish D3D12 execution support.

The same-schedule CPU/GPU RC comparison fixes bin centers at
`theta=2*pi*(j+0.5)/D_i`. Children 2j and 2j+1 have equal weight 1/2. Every stored
lower bin starts at `p_lower+r_i*d_parent`; a connector ends at
`p_upper+r_(i+1)*d_child`. The lower start MUST NOT vary with the child direction,
otherwise the stored interval origin would disagree with its next consumer.
For each child, trace all four bent connectors, compose each with its matching
upper interval, then apply each bilinear weight once. Only then average the two
children. Upper lookup surrounds the original lower probe position. Top bins
trace their own center direction to the actual boundary with the guard extension.
Equal-bin scalar illumination is mean radiance; diffuse/PBR normalization and
feedback remain additional work. No 16-byte storage is admitted before FP32
correctness; physical RC error also requires independent high-angular direct-ray
and analytic fixtures and cannot be waived by passing same-schedule comparisons.

The complete-fixture GPU RC qualification API has an explicit 4096-cell support
cap and requires the full fixture to fit its declared C0 tile. A larger domain,
incomplete tile or redundant cascade count returns unsupported scope before
allocation. This bounds the diagnostic C0 array to 1 MiB at its 32-byte FP32
record stride, with readback/direction/source/previous buffers counted separately.
This is a qualification API limit, not a product world-size, quality or memory
budget. It must be replaced by real bounded C0 scheduling before full-world
integration. Dispatch uses checked two-dimensional group packing and validates
all derived index/group ranges against device limits before GPU allocation.

The test-only CPU same-schedule evaluator retains tiny fixture levels for
comparison; it is neither a production fallback nor another GI architecture.
Numerical fixtures include one-cell axes, the common parent near origin at C1+,
constant-emission angular normalization and fixed-parity admission. A separate
1024-angle direct-ray room diagnostic reports physical scalar error and requires
zero light across a closed opaque separator. Passing implementation equivalence
does not establish general hard-shadow/aperture/phase acceptance. Half storage, feedback, production optical/matter mapping and publication remain
under qualification or unimplemented. The GPU hierarchy passed the bounded gates
recorded below; packed storage awaits its own gates.

### Completed FP32 baseline and GPU hierarchy evidence

Stages 3a/3b passed independent source review, including the FP32 crossing-event
rejection and source grouping corrections. The registered
`engine.tests.app.radiance_gpu_validation` entry passed on the RTX2080/Linux host
with the system-installed Khronos layer and both Normal Core Check and
GPU-Assisted Validation enabled. The loader recorded the layer in both instance
and device chains, without workspace overrides; the final unchanged-binary run
took 9.20 s. Evidence is retained in
`out/diagnostics/environment-lighting-20261101/radiance-gpu-validation-system-layer.log`
and its status file, with `review-fix-binaries.sha256` and
`review-fix-touched-source-hashes.sha256` identifying the pre-hierarchy baseline.
Earlier runs without actual layer availability do not establish that gate.
Focused asset validation passed; the broad `engine.tests.asset.gpu` shard timed
out after 450.14 s and is recorded in `3a-asset-gpu.log`, not counted as a pass.
This host evidence does not establish Windows/D3D12 execution.

The independent 1024-angle physical diagnostic gave blocked direct/RC radiance
0/0, open direct 2.5625 and RC 2.935546875: absolute error 0.373046875, about
14.6% relative error. That remains under qualification; numerical GPU/CPU
schedule equivalence is not permission to waive physical shadow/angular error.

The GPU hierarchy stage retains FP32 intervals and the existing pass/API.
Each immutable source snapshot owns one flattened `uint` flag buffer and a
16-byte scalar level table `{width,height,offset,blockShift}`. Level dimensions
are `ceil(W/2^l)` by `ceil(H/2^l)`, through 1x1. Level zero classifies only explicit
vacuum. Each later flag is true only when all four children exist and are proven
vacuum; padding, unavailable data, opaque cells and absorbing cells cannot skip.
GPU dispatches write disjoint level ranges with a global barrier between them;
no LDS or in-place reduction is used. Build roots bind only the RW flag view,
traversal roots only its SRV, followed by an explicit UAV-to-SRV transition before
trace/cascade work. All flattened node counts, offsets and two-dimensional
64-thread dispatch ranges are checked before allocation.

Tracing selects the largest proven vacuum block containing the ray-forward cell
and advances to its exact positive forward slab exit, clipped to the interval.
Opaque emission-before-stop, metre absorption/source integration, exterior and
nonprogress/error semantics remain unchanged. Every encoding rebuilds from the
captured GPU optical rows; caller mutations require explicit reprepare. Tests
compare direct rays with the independent oracle/CPU hierarchy, RC with the existing
test-only schedule, and captured input versus rebuilt edited input. Segment
observations count selected blocks/cells, not GPU time or a measured speedup.

Admission includes candidate and previous flags/metadata plus the unchanged
source, scratch, diagnostics and explicit reserve. The hierarchy costs
`4*sum(w_l*h_l) + 16*levelCount` bytes: 64x64 uses 21,844 flag bytes plus 112 table
bytes; 4096x1 uses 32,764 plus 208 bytes. These are the actual u32 tree costs, not
a v22 bitmask/jump-table estimate. The general numeric estimator keeps other
storage explicit; the pass adds these known resources to its admission and
largest-resource checks. The 4096-cell complete-fixture cap remains, with no
full-world, streamed C0, runtime publication, 16-byte storage or speed claim.

The hierarchy build and all five radiance leaves passed on this host. The routine
functional GPU entry took 10.82 s; the registered system Khronos core/GPU-assisted
validation entry took 12.43 s with no validation errors. The 64x64 vacuum fixture
observed one axis block span versus 64 plain-cell spans and 21,844+112 bytes of
flags/table. These are work/storage observations, not GPU timing improvements.
Evidence lives alongside the baseline in `hierarchy-build.log`,
`hierarchy-functional-gpu.log`, `hierarchy-radiance-gpu-validation.log`,
`hierarchy-source-hashes.sha256` and `hierarchy-final-hashes.sha256`. Unchanged CPU
optical/estimator evidence was reused. Independent review of the combined
hierarchy and packed-storage delta follows the coherent packed batch.

### Independent RGB half storage qualification batch

`GpuIntervalStorage::fp32` remains the explicit default diagnostic baseline;
`rgb16f` selects storage for the same trace/RC schedule and FP32 arithmetic.
There is no automatic fallback, alternative solver, shared exponent or scalar
transmission. Each 16-byte record contains four scalar uint words:
`{Lr|Lg<<16, Lb|Tr<<16, Tg|Tb<<16, status}`. Each channel is an independent IEEE
binary16 value. Trace, connector composition, bilinear weighting and angular
averaging stay FP32; only stored outputs are packed, and upper records expand
before use. Identity L=0/T=1 and opaque T=0 remain exact endpoints.

Source quantization remains as declared above. Storage now uses a narrow explicit
binary16 RTE codec built from uint/FP32 bit operations at packing boundaries,
rather than relying on an unspecified native conversion rounding mode. Normal
rounding retains 10 fraction bits with an even-tie decision; guarded subnormal
shifts are bounded to [14,24], including carry into minimum normal. Normal carries,
exact zero/one endpoints and subnormal normalization are explicit. Host and shader
decoding preserve sign, infinity and NaN payload information for domain/finite
validation. Physical trace/merge arithmetic and the independent test oracle are
unchanged. Codec instruction/time overhead is unmeasured; there is no speed claim.

The initial native-intrinsic diagnostic observed values consistent with truncation
for both normal/subnormal ties and non-ties: FP32 0x33c00000 became half 0x0001
rather than RTE 0x0002, 0x3f803000 became 0x3c01 rather than 0x3c02, and
0x38d1b717 became 0x068d rather than 0x068e. Exact runtime RHI SPIR-V was not
captured, so this is observed output, not an attribution to hardware or compiler.
The preserved `packed-aggregate-single-leaf.log` and associated source/binary
hashes record the failing baseline. The accepted correction keeps the desired
RTE contract instead of changing its oracle to the observed native behavior.

Accepted direct round-trip error is at most one half ULP, with RTE subnormal
rounding bounded by 2^-25. Qualification compares exact packed bits and decoded
values against an independent arithmetic reference, covering both tie directions,
normal mantissa carry, the subnormal/normal boundary, 65504 radiance and weak
independent secondary channels. The deterministic codec passed the bounded
execution gates recorded below.
Positive underflow is measured storage error, not an inferred optical opacity or
cutoff. Nonfinite or out-of-domain values, and radiance beyond finite half range
65504, set failed per-record status; completion returns an error. Values are never
clamped or silently replaced with FP32. Upper status propagates through every
dependency.

Status occupies the fourth interval word on all levels. Local connector-span
counts, excluding ancestor traversal, occupy only one separate bounded uint plane
for C0 plus the selected C1 diagnostic; upper levels allocate no general count
arrays. Direct traces use rayCount count words. Readback appends this plane to
the packed intervals, so it is 20 bytes per returned record, not a hidden FP32
readback. Host reporting expands decoded values after the completion gate. Qualification
snapshots also retain the existing raw packed words for exact bit-validation and
mismatch diagnosis; this adds no GPU storage or readback bytes.
Reflection verifies 16-byte field offsets/stride and the device buffer descriptor
is checked against the selected storage stride.

Admission uses actual 16-byte even/odd parity capacities plus
`4*(C0records+1)` GPU diagnostic bytes and `20*(C0records+1)` readback bytes,
with unchanged optical/direction/hierarchy buffers, candidate+previous lifetime
and explicit caller reserve/largest-resource ceilings. Direct traces count
16-byte output, 4-byte diagnostics and 20-byte readback per ray. Single-cell
buffer-only admission is 124 bytes, or 248 during replacement, including optical,
ray and hierarchy buffers; these fixture checks do not define a product budget.
The general estimator remains format-size driven. Neither the 4096-cell complete
fixture guard nor its current C0 allocation claims full-world scheduling.

Matched FP32/packed tests report maximum L/T error separately from the 14.6%
physical RC diagnostic. A test-only same-schedule reference quantizes every
stored level independently; weak media, multiple levels, bright saturated/weak
RGB sources, near-opaque runs and transmission before a bright far source expose
accumulated storage error. One adjacent half quantum plus the established FP32
arithmetic tolerance accommodates rounding ties in implementation comparisons;
that tolerance is not a blanket physical-quality threshold. The native-intrinsic batch failed the RTE contract; the deterministic codec
correction passed source review, Debug ordinary and actual system Khronos/GPUAV
execution, and a Release build with 15 focused CPU and eight GPU checks. Evidence
is retained in `rte-source-hashes.sha256`, the `rte-*-binary.sha256` snapshots and
`rte-*-status.txt` files under the existing diagnostics directory. The first
native-intrinsic failures and aggregate probe remain preserved. This qualifies
the bounded format implementation, not physical RC quality or world scheduling. Game/display integration and whole-world
physical/angular acceptance remain separate work.

### Authored material optical bridge

The current metadata batch uses the existing selected material catalog and
presentation capture. `MaterialAppearance` optionally declares
`[appearance.transport]` with required `contract_version=1` and explicit
`model='vacuum'|'opaque'|'absorbing'`. RGB `sigma_r_per_m`, `sigma_g_per_m` and
`sigma_b_per_m` default to zero and are extinction per world metre. Absence stays
unavailable. Alpha, IOR, texture coverage and physical phase never select a model
at runtime. Existing visual-only fixtures with unavailable optics remain valid;
strict optical conversion rejects unavailable rather than substituting vacuum.

The source function S is the existing linear emissive RGB multiplied by existing
strength. Opaque emits S once before stopping and has zero sigma. Absorbing uses
j=sigma*S, so a segment contributes S*(1-exp(-sigma*d)) per channel. Vacuum has
zero sigma and source. There is no scattering or transport refraction. The
numerical `OpticalCell` oracle retains its more general zero-sigma positive-j
volume-emitter case; authored materials intentionally have the narrower source
function model. Shared game validation covers descriptor imports and typed
projection, checks original and quantized float ABI products before narrowing or
multiplication, and rejects positive underflow, overflow and incoherent unused
quantities.

The same selected material ID carries this metadata through `MaterialDefinition`
and `captureCatalogPresentation_` into the existing `GridTerrainVisual` palette.
The append-only ABI is 96 bytes: versioned model uint at 80, scalar sigma at 84/88/92,
with all previous offsets unchanged. Tags are 0 unavailable, 0x101 vacuum_v1,
0x102 opaque_v1 and 0x103 absorbing_v1. The particle palette carrier mirror also
uses 96 bytes. Actual program reflection, upload elementSize/size checks and the
isolated grid-shader compute probe qualify the same layout and S/j mapping; no
production draw pipeline invokes the probe. Raw material metadata is not another
transport catalog. Presentation fingerprints include this version and optical
appearance; numeric recipe/worldgen/simulation fingerprints remain unchanged.

A dense 65536-row palette increases from 5 MiB to 6 MiB per owner, +1 MiB. Existing
active/candidate/retiring lifetime overlap and GPU completion govern both owners;
two overlapping palettes account for 12 MiB. This does not define a new standalone
budget or admit full-world lighting allocation.

All 63 shipped material TOMLs explicitly author the following artistic starting
policies, with existing source factors unchanged. These coefficients are not
measured optical properties or final brightness; scene inspection after actual
light-field integration must guide later tuning.

| Material | Model | RGB sigma per metre |
|---|---|---|
| clear_quartz | absorbing | .04,.03,.02 |
| glass | absorbing | .03,.02,.015 |
| diamond | absorbing | .02,.02,.02 |
| ice | absorbing | .10,.04,.02 |
| polluted_ice | absorbing | .9,.35,.8 |
| crystal | absorbing | .9,1.8,.35 |
| water | absorbing |1.5,.12,.06 |
| brine | absorbing | .35,.12,.08 |
| polluted_water | absorbing |1.8,.6,1.2 |
| crude_oil | absorbing |4,6,9 |
| steam | absorbing | .06,.06,.06 |
| polluted_oxygen | absorbing | .035,.015,.04 |
| chlorine | absorbing | .08,.025,.10 |
| oxygen, hydrogen, natural_gas, carbon_dioxide | vacuum |0,0,0 |
| Remaining 46 named solids/materials, including magma | opaque |0,0,0 |

Magma is opaque despite visual blend and keeps its amber source; crystal and
water retain their violet/cyan source factors. The current batch adds no terrain
contour reconstruction, matter optical density/geometry, publication/invalidation
scheduler, scene GI activation, gameplay illumination or postprocess integration.
The metadata bridge passed independent boundary review, Debug/Release builds of
the game and app/asset/game test targets, 73 Content/Pack leaves, focused
grid/lighting checks, nine radiance GPU checks, two existing terrain/particle
raster leaves, and both shipped world-profile validators. Debug additionally
passed the 33-leaf functional GPU shard and the nine-leaf opt-in radiance entry
with the system Khronos Core and GPU Assisted validation active. Release GPU
checks used ordinary execution, not GPU Assisted validation. Final source and
binary identities and commands/statuses are retained under
`out/diagnostics/environment-lighting-20261101/material-final-*` and
`material-release-*`. The earlier broad asset GPU shard's 450-second timeout
remains recorded; these focused results do not establish a full asset-suite pass.

### Rendered terrain geometry qualification (in progress)

The next bounded qualification step preserves the renderer's noise-free quadratic
outer curves and same-label saddle connectivity. The shared math case/point
tables supply topology; material roundness supplies the quadratic control point.
The straight, disconnected saddle geometry used by physics/matter is not silently
substituted for these rendered contours. Visual boundary noise, AA width,
decoration and texture alpha remain outside transport.

An owning numeric snapshot copies same-ID `OpticalCell` rows, roundness and
centre labels. The initial API supports opaque/vacuum fixtures only; absorbing
or unavailable selected material inputs explicitly fail. Opaque termination
needs no internal material-interface integration: source selection at the hit
uses the existing deterministic rendered point-label rule, including four-label
junctions. The world exterior is explicit vacuum or black opaque, separately
from the halo corner labels that shape in-world edge contours. The CPU oracle
enumerates analytic ray/quadratic events; GPU traversal uses the same geometry
with explicit failure for uncertifiable numeric intersections. This is not yet
validated, full-world source reconstruction or scene activation.

Conservative FP32 intersection certificates require controlled arithmetic order.
The pinned Slang v2026.17.1 predates the per-variable `precise` SPIR-V fix in
[upstream PR 12208](https://github.com/shader-slang/slang/pull/12208).
The accepted remedy compiles only the new terrain programs with the existing
component-level precise option and gives that already-linked component to RHI.
It does not change global shader defaults or old cell-constant programs.
Qualification must inspect the same linked component's SPIR-V and separately
execute it. Native division/square-root accuracy and denormal behavior must be
covered by the certificates; a rounded tangent or collapsed positive span must
report numeric ambiguity instead of becoming a successful miss.

The source implementation is now authored and awaiting the serialized execution
and review gates. `renderer.optical_geometry` owns the copied numeric fixture
snapshot and analytic-double Bezier-event oracle; `RadianceCascadePass` adds
`prepareTerrainTrace` and `prepareTerrainCascades` using its existing submission,
fence, quarantine and readback protocol. The qualification cap remains 4096
centre samples, with no two-label cap: each dual cell naturally has at most four
opaque source candidates. Label zero must have an explicitly supplied vacuum
row; missing IDs and the reserved 65535 sentinel fail capture. Absorbing and
unavailable rows return `not_supported`. There is no game activation or material
catalog duplication.

Dual origins are `(i+.5,j+.5)` with a one-sample halo, then clipped to the declared
world box. Halo material labels shape the half-cell in-world edge. Traced
exterior is independently declared vacuum or **black** opaque; halo emission
never becomes incoming environment light. Noise, AA/alpha and decorations do
not alter this macro contract. Numerical tangent/collinear uncertainty returns
`argument_out_of_domain`; an analytically isolated zero-length contact does not
create an opaque path interval. The CPU oracle also rejects near-degenerate
rounded discriminants/root collapse through explicit floating-error budgets;
its use of double is not a claim of exact real arithmetic.

GPU occupancy uses the equivalent corner conic
`F=x+y-.5-.25*r+r*(x-y)^2`, reflected for the other corners, and the existing
saddle union/complement topology. The CPU event solver remains independently
parameterized by Bezier arcs. Conservative whole-span field bounds precede the
complete root path; only proved uniform spans avoid root solving. Ambiguous
root signs/order, interval edges, DDA ownership or positive spans return named
shader status 32 (`numeric_ambiguity`), propagated through RC and surfaced as
`argument_out_of_domain`. No epsilon repair, root merging or silent cap is used.
The scalar dual-cell traversal has a width+height+2 bound; exhaustion is an
error. Terrain currently uses no vacuum hierarchy rather than incorrectly
reusing cell-box vacuum flags for rounded dual occupancy.

Only the new Vulkan terrain entry programs are linked with component-level
`FloatingPointMode=PRECISE`. Other backends explicitly return `not_supported`
for this qualification API; the existing cell/packed paths retain their prior
compile mode. The exact pinned v2026.17.1 release is commit ca6e0a657 (11 September
2026), before PR 12208's 23 September merge. Its
[pinned emitter](https://raw.githubusercontent.com/shader-slang/slang/v2026.17.1/source/slang/slang-emit-spirv.cpp)
consults only the effective floating-point mode for `NoContraction`, confirming
that a local `precise` qualifier alone is insufficient. RHI's zero-entry-count
already-linked-component path retains that same component. Explicit
`writeTerrainSpirvForQualification` writes its exact three entry binaries for
inspection; the GPU test invokes it only when
`PPR_TEST_TERRAIN_SPIRV_DIRECTORY` names a diagnostic directory. Emitted IR and
actual execution remain separate gates.

The local normal-FP32 certificates enclose correctly rounded add/multiply,
reject subnormal/FTZ-zero and nonfinite arithmetic, and conservatively expand
normal-domain division by eight adjacent representations to cover Vulkan's
[2.5 ULP requirement and binade asymmetry](https://docs.vulkan.org/spec/latest/appendices/spirvenv.html).
Square roots use bounded positive-float squared bracketing, without assuming
native sqrt rounding. Near-degenerate input roundness that could flush an
average into a straight arc is rejected before GPU allocation. These safeguards
may reject otherwise useful queries; their frequency and GPU cost must be
measured before production admission. No speed or full-world availability claim
follows from this bounded implementation.

Additional terrain buffers are 8 bytes per captured sample plus 64 bytes of contour
points and 1152 bytes of cases, on top of the existing 40-byte numeric source rows.
The 32-byte terrain constants use the existing immutable root/driver reserve
policy. Admission counts the actual selected buffers and previous live batch,
per-resource ceilings, parity capacities and bounded diagnostics/readback.
No packed terrain storage is selected. Focused tests cover analytic rounded hits,
all 16 masks at roundness 0/.5/1, repeated/distinct saddles, four-label source
selection, a one-cell wall/door across a global seam, immutable edits, explicit
black exterior and near-tangent/collinear ambiguity. Terrain RC is compared to
the same selected schedule independently from the physical angular-quality
reference. Finite 8→16 angular tuning remains the next batch, after geometry
execution/review; the prior 14.6% physical diagnostic is still unaccepted.

### Accelerated DDA contract

The historical `docs/oni_v22.slang` source is absent from this checkout on
1 November 2026. The reference locations above record prior analysis, not a
source port verified during this implementation. Obtain that exact source before
claiming reuse, or independently implement and validate the documented hierarchy.
Treat v22's hierarchy as a useful prototype, not a drop-in transport primitive.
The table describes safe exits from aligned blocks, not an isotropic distance
field. Prove forward progress for negative/axis-aligned rays, exact boundaries,
corner ties and world edges. Audit LDS read/write ordering and inter-dispatch
barriers before preserving the mip reductions.
The prototype's in-place LDS reduction needs separate scratch or a phase that
finishes all participating reads before a group-wide barrier and writes. A
barrier only after writes cannot protect those earlier reads. In packet traversal,
preserve each ray's hit axis and stop newly hit lanes before zero-sized skip
arithmetic. Benchmark packet register pressure and divergent ray lengths; four
rays per invocation is not an established improvement.

Return integrated radiance/transmittance over the requested interval, not just
the first material hit. Vacuum blocks may be skipped; opaque segments terminate
remaining transport after accounting for their boundary/source contribution.
Transmissive/absorbing segments integrate by physical path length. Nonopaque
emissive or absorbing cells must not be classified as skippable vacuum. Define
the same contour and matter coverage convention for the oracle and RC.

V22's fixed 128-step cap must not silently turn exhaustion into a miss or clear
transmittance. Prove an adequate traversal bound or expose incomplete work and
resume it; keep the previous valid lighting output until completion. An optional
early-out on small remaining throughput requires an explicit error threshold.
Validate the accelerated result against plain cell DDA before vectorizing rays.

The initial CPU `OpticalHierarchy` captures an owning immutable optical snapshot.
A cell is skippable only when explicitly vacuum; an aligned parent is skippable
only when all four children exist in-domain and are skippable. Opaque, absorbing,
volume-emitting and unavailable data are never classified as vacuum. Edits require
explicit recapture; old owners remain valid snapshots and cannot silently observe
mutated caller storage. This is independently authored; it does not port the
unavailable v22 file and does not yet implement an incremental GPU jump table.

`traceOpticalAccelerated` uses scalar hierarchical DDA. At each cursor it binary
searches monotonic grid-plane crossing events to select ray-forward cell ownership,
including negative rays and corner ties, then exits the largest containing aligned
vacuum block or integrates one nonvacuum cell. The initial locator costs
`O(log extent)` per axis per visited segment; no timing improvement is claimed.
Every positive segment ends at a new X/Y plane or the interval end, so at most
`width+height` segments suffice. Nonprogress or a violated bound returns an error,
never a partial successful interval. No epsilon, 128-step cap or throughput cutoff
is used. Aggregate capture/trace profiling scopes and visited-segment/vacuum-block
observations are available without per-cell instrumentation.

CPU parity fixtures exercise translated metre grids, non-power-of-two bounds,
positive/negative/axis/near-axis directions, boundary/corner ties, subintervals,
explicit exterior policies, immutable rebuild after emitter insertion/removal,
and a 513-cell absorbing/emitting corridor with independent analytic expectation.
These establish the declared cell-constant CPU model only; GPU traversal, contour
and matter geometry, optical edit propagation and RC qualification remain open.

Rebuild affected hierarchy ancestors and jump-table dependencies after optical
edits. Prove the dirty region; local terrain changes can alter skip classifications
outside the edited cell. Time the acceleration rebuild as part of the solver.

### RC memory admission

Before full-domain allocation, enumerate `sum(levelProbeCount * levelDirectionCount *
storedBytesPerInterval)`. With a common 2D layout that quarters probe count and
quadruples direction count each level, their product stays roughly constant;
upper levels are not automatically cheap storage. Include the near-probe spacing
and base angular count in the estimator. The exact implemented layout and live
ranges decide memory; accelerated traversal alone changes none of these counts.

Include interval extensions, padding, retained reconstruction inputs, feedback,
candidate/published generations, readback rings, terrain assets, multisampled
HDR attachments, motion/validity inputs and output-resolution temporal histories.
Check both total live memory and individual resource limits on supported devices.

The initial numeric estimator fixes probe phase to `p_i(k)=0.5+2^i*k` in fine
cells and chooses upper bilinear probes around the original lower probe position,
not the ray endpoint. Recursive floor/ceil parent support for C0's in-domain
centers is `k=0..ceil((extent-1)/2^i)`. The nominal upper maximum uses `floor`
instead: there is no low guard and at most one high guard per axis. Even coarser
levels therefore keep two probes when required rather than stretch or clamp their
lookup. This derives support for C0 probe queries only; occlusion-aware final
surface reconstruction and connector angular mapping remain qualification work.
The estimator requires the selected nominal outer radius to cover the world
diagonal. This is insufficient for padded upper probes outside the world: for
4096², a top guard at `(8192.5,8192.5)` is farther than 8192 cells from the origin.
The top interval therefore traces from nominal near to actual world exit for every
padded probe/direction, including explicitly defined outside transport. The result
reports a separate conservative terminal far bound derived from the maximum axis
distances between padded-probe corners and world corners. It uses their exact
integer L1 sum rounded up, which bounds Euclidean distance on every supported
host without floating-point underestimation. The 4096² bound is 16385 cells;
individual rays stop at their actual exit. Nominal intervals, cascade count and
storage are unchanged. This extension adds trace work and requires angular/error
requalification; it is not assumed free or quality-neutral.

Every cascade reports nominal/padded probes, guard counts, directions, interval
bounds, logical records, resident records and storage bytes. The selected solve streams a C0 tile and logically keeps adjacent cascades while
solving coarse to fine. Actual allocation reserves maximum even-level and
maximum odd-level capacities separately; admission uses their sum, not only
theoretical maximum adjacent live bytes. Published/fence-retired C1 fields are
counted separately. Optical/scalar outputs count all caller-declared live generations.
The sum of all levels per generation is reported separately as a diagnostic,
not confused with the selected streaming peak. This is an integer accounting
model, not an implemented GPU allocation schedule or proof of aliasing safety.

Callers must supply record sizes, explicit lighting/total admission budget,
individual resource ceiling, all other simultaneously live bytes and the largest
individual resource among that reserve. That reserve includes feedback,
hierarchies, directional visual outputs, terrain, MSAA/display history, readback,
alignment and driver headroom as applicable. A missing reserve is not proof that
those resources are free. Checked arithmetic rejects overflow and insufficient
reach. Budget/resource failures retain the computed demand and return an explicit
admission error before any GPU allocation; zero budget admits nothing.

| Example 4096² output alone | Bytes before overhead |
|---|---:|
| R16 scalar | 32 MiB |
| R32 scalar | 64 MiB |
| RGBA16F | 128 MiB |
| Four RGBA16F fields | 512 MiB |
| V22 three-level visibility bitmasks | Approximately 2.625 MiB |
| Full-world R32UI jump table | 64 MiB |
| Eight-bit jump values, if supported or explicitly packed | 16 MiB |

For illustration, v22's two-cell base probe spacing and four base directions
produce approximately one interval per world cell per level. Six levels consume
768 MiB at eight bytes per interval, or 1.5 GiB with two RGBA16F values per
interval for RGB radiance/transmittance, before other resources. These are layout
calculations, not selected quality settings. Independently prove the cascade
lengths cover the required transport reach; six levels are not automatically
enough for a full 4096² world.

Directional output also needs its own estimate: nine signed RGB coefficients in
nine padded half4 values take 72 bytes per probe, or 288 MiB at two-cell spacing
on a 4096² world. Double buffering and fence-retired generations add to this.
The compact scalar gameplay cache does not make these angular dependencies free.

### Planned cascade lifetime and finest-level streaming

Use coarsest-to-finest construction/merge. Once a level has consumed the upper
level, release or alias upper scratch only after its GPU readers finish. Retain
a complete globally merged C1 for each published lighting epoch. Evaluate C0 in
bounded world tiles, reducing immediately into full-map scalar output and the
next diffuse source field. Avoid allocating a complete C0 interval array.

Retain directional C0 bins only in a bounded world-anchored visual tile cache.
Generate missing visual tiles from the completed C1 plus its matching optical,
emission/bounce-source and boundary epoch. This may run before shading in the
same GPU submission with explicit barriers; no CPU fence wait is needed. Do not
silently substitute scalar illumination or a coarse C1 sample on cache misses.
Budget worst-case zoom-out requests; if the visible set exceeds cache capacity,
derive bounded generation/shading batches with safe lifetimes rather than making
the cache unbounded or treating missing tiles as dark.

Hold the required optical/source snapshots immutable until every corresponding
C0 reconstruction and reader retires. New geometry combined with old C1 is not
coherent. Publish a full scalar/feedback generation only after its required C0
sweep completes. Subsequent CPU chunk delivery carries individual revision/age;
do not label a partially refreshed gameplay cache uniformly current.

For the proposed one-cell/eight-direction doubling layout, ideal unpadded counts
are `C0 = 8N`, `C1 = 4N`, `C2 = 2N`, with `N = 4096²`:

| Allocation | 16-byte intervals | 12-byte intervals | 8-byte intervals |
|---|---:|---:|---:|
| Full C0, avoided by streaming | 2 GiB | 1.5 GiB | 1 GiB |
| Merged C1 | 1 GiB | 768 MiB | 512 MiB |
| C2 | 512 MiB | 384 MiB | 256 MiB |
| C1 plus C2 working pair | 1.5 GiB | 1.125 GiB | 768 MiB |
| Published C1 plus candidate C1/C2 | 2.5 GiB | 1.875 GiB | 1.25 GiB |

These exclude guards, scratch, epoch sources, outputs, retirement and display.
Retaining all ideal levels at 16 bytes would approach 4 GiB. With fourfold angular
growth, each ideal level instead holds `8N` intervals: 2 GiB per level at
16 bytes. Its adjacent-level pair alone is 4 GiB. Reject inadmissible tuning
layouts before allocating. The bilinear fix requires extra connectors, but their
four weighted contributions can be reduced within a dispatch/tile rather than
stored as four permanent copies of each merged interval.

Read-only padded-layout preflight on 1 November 2026 gives the following demand
for 4096², 16-byte intervals and an illustrative 16² streamed C0 tile. The tile
size is not an admitted production choice. Counts use the estimator's recursive
floor/ceil support and fixed even/odd capacities; the alternative schedules are
arithmetic projections, not implemented GPU configurations.

| Base directions / angular and range growth | Cascades | Candidate parity scratch, MiB | One retained C1, MiB | Combined, GiB |
|---|---:|---:|---:|---:|
| 8 / doubling |14|1538.001|1025.000|2.503|
| 16 / doubling |14|3076.001|2050.000|5.006|
| 8 / fourfold |8|4290.500|2050.000|6.192|
| 16 / fourfold |8|8581.000|4100.001|12.384|

These exclude optical/source snapshots, hierarchy, scalar outputs, feedback,
directional tiles, readback, retirement beyond the one retained C1, and reserves.
The current qualification optical row alone would cost 640 MiB per full-world
snapshot at 40 bytes/cell; production snapshot representation remains undecided.
The current u32 vacuum pyramid would cost about 85.33 MiB per snapshot. Fourfold
angular growth offsets the fourfold reduction in ideal probe count per level;
padding makes coarse levels larger, rather than cheap. No layout is admitted
against the GPU's physical 8 GiB capacity by these calculations.

The selected 1080p MSAA4 scene color/depth with 4K RGBA16F histories would require
at least 474.609 MiB for RetroPass's three slots, before motion/identity inputs,
TSR intermediate outputs, bloom, swapchain, UI or driver reserve. Its current
scene-resolution histories instead occupy 332.227 MiB with the same scene
attachments; this preflight does not claim the selected TSR is implemented.

These are illustrative layouts, not a selected directional format or a complete
RC allocation estimate. Cascades can require multiple GiB. Reusing scratch
helps only where dependencies permit;
prove live ranges rather than assuming two ping-pong buffers suffice.

**Full-map RC admission is conditional on a measured memory/time budget.** Lower
update frequency does not lower peak allocation. Mostly solid cells do not
reduce dense RC allocation automatically. First evaluate admitted RC
layouts, precision and scratch reuse. Exact dependency-tiled/sparse execution is
a separate engineering milestone if subsequently justified, not a promised
fallback. If RC does not fit, full-map support remains unmet until a
measured engineering result or product decision changes the constraints. A finite
transport radius with halos changes the lighting model and needs explicit
acceptance. Coarse grids must not silently erase one-cell doors and tunnels.

### Interval storage formats

Keep interval radiance/transmittance separate from signed directional-basis
coefficients. A positive interval can use unsigned radiance packing; a harmonic
coefficient can be negative even when the represented lighting is nonnegative.

| Representation | Stored interval bytes | Supported meaning |
|---|---:|---|
| Two float4 values | 32 | Small-domain FP32 numerical baseline: RGB `L`, RGB `T`, reserved components |
| Two half4 values | 16 | Initial production target: independent half channels for RGB `L` and RGB `T` |
| One half4 | 8 | RGB `L` plus scalar `T`; only achromatic transmission |
| Six independently packed half channels in uint3 | 12 | RGB `L` and RGB `T` without shared-exponent loss |
| Packed RGB9E5 `L` plus three half `T` channels in uint3 | 12 | Compression candidate with explicit packing/padding and independent colored transmission |
| Two packed RGB9E5 values | 8 | Aggressive candidate; both radiance and colored transmission share exponents within their respective triples |

Start FP32 arithmetic with a 16-byte half-storage target after comparison to the
32-byte oracle. A `half` declaration alone is not proof of 16-bit storage;
assert actual byte layout and inspect bindings. RGB9E5 is an optional RC storage
optimization, not a second solver. It has no alpha or sign and shares an exponent
across RGB, so a bright channel can erase weak channels. See the
[DXGI format definition](https://learn.microsoft.com/en-us/windows/win32/api/dxgiformat/ne-dxgiformat-dxgi_format).
Do not use it for signed harmonics or silently replace RGB transmission with alpha.

For portable compute storage, evaluate explicit uint packing, decode before
arithmetic/interpolation and repack at storage boundaries. Never interpolate
packed integers. Native-format UAV access is a capability check, not an assumed
baseline; DirectX documents it as an
[optional format feature](https://microsoft.github.io/DirectX-Specs/d3d/D3D12R9G9B9E5Format.html).
Include packing instructions, bandwidth and buffer stride in performance results.

Test bright saturated emitters with weak secondary channels, many weak absorbing
segments, long-path feedback, black/clear intervals and near-opaque transmission.
Accumulate each trace/merge in FP32; measure cumulative quantization across levels
and bounces. Preserve exact identity/opaque endpoints, detect nonfinite values
and overflow, and define representable emission limits rather than silently
clamping HDR energy. Select compression only if it meets the same error gates.
For compressed `T`, measure its effect after multiplication by bright distant
sources, not only absolute transmittance error.

## Sparse RC within the same solver

Sparse RC is relevant to this mostly solid world. The paper's §3.5 flags
requested finest probes and recursively includes their bilinear parents; its
implementation reduces computation while retaining dense storage. It treats
excluded media as transparent, which is not valid for PPR's opaque terrain.

PPR must separate **receiver/output sparsity**, **transport geometry**, and
**allocation sparsity**. Omitting an output probe does not remove any blockers,
emission or absorption encountered by an active ray. Keep the world optical
representation available across the full domain. Seed active finest probes from
all required gameplay receivers, material/bounce receivers, exposed surfaces
and visual reconstruction requests, independently of the camera. Define query
semantics for points inside opaque material separately from available air/surface
illumination; do not disguise missing outputs as valid darkness.

Keep probe coordinates and angular schedules on the same fixed lattice. Recursively
include every parent, angular child and boundary/connector dependency needed by
the corrected merge. A probe inside a solid cannot be omitted merely because it
is inside a solid: its interval can begin beyond that solid and serve another
receiver. Initially compute all directions for each active probe. Adaptive probe
relocation or reduced directional coverage would require a new reconstruction
derivation and is outside this baseline.

Deliver sparsity in two distinct stages within RC:

1. Compact active work while keeping the validated addressing/storage layout.
   Compare against the full active-probe version on small/medium domains. Include
   receiver discovery, closure construction, compaction and edit updates in cost.
   This stage saves work, not necessarily memory.
2. If required by measured memory, page/pack the active probe tiles using explicit
   indirection and completion-safe allocation. Unallocated entries mean missing
   data, never identity transport. Admit the complete dependency closure before
   solving; capacity failure retains the previous coherent result and reports
   the unmet budget. Count page tables, padding, fragmentation and overlapping
   epochs. This stage must demonstrate actual allocated-byte reduction.

Opening doors, digging chambers and adding emitters can expand the receiver and
dependency sets. Recompute conservatively before publishing the changed epoch;
reuse pages only after their last reader completes. Keep offscreen receivers
progressing. Upper cascades may become nearly dense even with sparse finest
receivers, so do not extrapolate memory savings from the percentage of air cells.
Test large excavations and all-air stress scenes for capacity and latency.

Use fixed-layout streaming first to reduce peak storage without sparse topology.
Treat paged storage as a measured RC optimization, not as a guaranteed way to fit
any world. Sparse RC does not authorize chunk-isolated solves, finite transport
radius or camera-only gameplay coverage.

The [Sparse RC 2D prototype](https://github.com/icekontroi/sparse_rc_2d) is useful
prior work for task generation. It uses zero interpolation and documents dense
auxiliary resources, compression error and task-utilization limits. Its results
are not proof that the corrected bilinear PPR merge inherits the same savings.

## Ownership and integration

Use the current owned publication path, not a parallel naked world texture:

- `game/colony/SimFramePacket.cpp`: game-to-render owned numeric data translation.
- `lib/engine/app/renderer/App.Renderer.GridPass.cppm` and `.cpp`: world material
  resources, cell publication, neighbor tables, contour geometry and fence-safe
  resource generations.
- `lib/engine/app/App.Application.Editor.cpp`: owner-lane render publication,
  encoding and world/UI ordering.
- `lib/engine/app/renderer/App.Renderer.Types.cppm`: pre-render encoder contract.

Propose an `engine.app:renderer.grid_lighting` partition coupled to GridPass's
world bundle, plus a pass-owned display component. They consume numeric cells,
materials and publication identities without importing sim types. Game code
owns gameplay scheduling and delivered illumination values. Renderer stays
content-free and owns device submission/presentation only.

Register new module declarations, implementation files and umbrella exports
together in `lib/engine/app/CMakeLists.txt`. Put optical shared declarations,
RC construction/merge/reconstruction, output resolve and post-processing shaders
under `assets/shaders/`; verify the existing staging path for affected targets.
Compile through `IShaderService`; validate reflection and mirrored CPU/GPU layouts.
Use the actual Slang-RHI `ad1b92f` / Slang `2026.17.1` pin in
`cmake/external/SlangRHI.cmake`. Recheck it at implementation time.

Never mix optical cells, materials or neighbor tables from different published
cohorts. Extend existing publication identities and retirement rules instead of
creating independent lifetime guesses. Respect row-major, row-vector camera
matrices; separately document the convention of any color-transform matrices.

## GPU schedule and persistent updates

Start with one graphics queue and explicit inter-dispatch barriers:

1. Capture a coherent published world/material/optical generation.
2. Prepare optical inputs and changed occupancy summaries.
3. Construct and merge RC intervals; reconstruct candidate outputs.
4. Resolve directional radiance and gameplay scalar illumination.
5. Form next diffuse source feedback from the previous complete state.
6. Publish candidate lighting only after its submission completes.
7. Copy bounded gameplay results through completion-tested staging slots.
8. Compose the shared parallax backdrop and rasterize the world into multisampled
   linear HDR, preserving samples for custom reconstruction.
9. Run MSAA temporal reconstruction and display processing, composite to the
   surface and draw UI.

This is a dependency schedule spanning submissions, not a demand to wait on the
CPU mid-frame. Render using the last completed lighting generation while newer
work is in flight; publish completion and enqueue readbacks on subsequent
owner-lane progress without an immediate blocking fence wait.

Close offscreen render passes before subsequent compute passes. Group barriers
cannot replace inter-dispatch dependencies. Do not recursively submit/present
from an encode callback. Stamp all resource participants when submission is
accepted, including when presentation subsequently fails. Retire replaced
resources only after their last reader's fence completes.

Keep previous and next bounce sources separate. First solve tests emission-only
transport; successive feedback generations add diffuse bounces. Include emission
once, define reflectance/normalization and convergence, and reset invalid feedback
when geometry or emission changes. A single cascade evaluation does not supply an
unlimited converged multi-bounce solution.

Where resource admission permits, begin with coherent low-frequency full-domain
generations. Hold inputs stable during time-sliced work, finish bounded work and
queue/coalesce newer edits so continuous editing cannot starve publication.
Retain the previous completed field with explicit age while building the next.

Invalidate transport dependencies after edits, not merely the edited storage
chunk. Cover opened/closed doors, distant corridors, removed emitters and material
reloads. Separate terrain acknowledgments from lighting completion. Reject old
solves/readbacks on world replacement. Define enclosed-solid samples and unknown
samples separately. Maintain first-write validity without blending against zero.
Initially invalidate the complete solve conservatively and finish bounded cohorts;
introduce partial dependency closures only after equivalence is demonstrated.

World lighting must progress without a presentation target. Current
`ApplicationEditor::submitRenderWork_` drops no-target frames and
`executeRenderWork_` returns before GPU work without a render target. Add bounded,
coalescing owner-lane maintenance requests and a content-free encoder submission
path with the existing admission/fence/settlement semantics. Schedule via explicit
game/simulation time. Maintenance must also carry and apply pending owned
terrain/material/optical updates and return their ordinary publication
acknowledgments; recomputing retained GridPass inputs would leave lighting stale
and could stall edit publication. Preserve the same candidate/cohort identities
and cancellation behavior as surface-backed work.
Minimized interactive operation still uses the GPU; GPU-less
headless operation is a separate capability decision, not valid darkness.
Shutdown disables requests, cancels queued work, drains accepted submissions and
then releases resources in dependency order.

## Gameplay queries and directional shading

Gameplay reads camera/exposure-independent mean incoming luminance with documented
angular normalization. Return value, validity, evaluated simulation tick and
world/material/optical generations; expose age. Do not label values lux without
calibration, or treat scalar brightness as a replacement for line of sight.

Maintain a game-owned compact full-map cache, updated from bounded changed-chunk
readbacks. No blocking immediate map or per-frame whole-map transfer. Busy rings
retain/coalesce pending revisions; they do not mark undelivered values complete.
Reject stale results. Sim can consume plain numeric values without depending on
GPU/app types. Qualify the adopted one-second age target at normal speed before
full gameplay acceptance; fast-forward behavior needs separate measurement.

Rendering needs directional data beyond this scalar cache. Preserve a reference
directional representation before evaluating compact signed bases. RGB9E5 cannot
store negative harmonic coefficients. Compare reconstruction error and memory
cost before selecting the production representation.
Initially retain actual C0 angular bins in visual tiles. Do not automatically
port v22's nine-coefficient harmonic cache: eight angular bins do not determine
nine independent Fourier coefficients. Any richer reconstruction from unaveraged
children must account for its own data and cost. Qualify basis compression
against angular bins rather than assuming a smooth basis preserves hard shadows.

Define a deliberate planar-to-3D incident-direction reconstruction for normal-map
PBR. Test front-facing, bevel and rotated normals under rotated emitters. This is
2D transport with a 2.5D surface response; it cannot recover absent elevation or
guarantee accurate sharp 3D reflections. Start diffuse plus rough specular, with
the approximation documented. Macro geometry governs transport; authored detail
normals govern visual BRDF response initially, not camera-filtered bounce energy.

Filter using world-space pixel footprints. Avoid averaging illumination across
occluding walls or averaging occupancy until thin walls disappear. Optional
screen-space refinement uses world geometry and replaces the near interval with
the world field as continuation. Do not add a second full GI evaluation on top.
An exact short near-field trace cannot restore distant aperture visibility or
source information already blurred in that continuation. Include any refinement
needed to pass shadow requirements in RC qualification's measured cost.

## Display chain

The selected AA/upscaling method is John Hable's
[Temporal Super Resolution via Multisampling](https://filmicworlds.com/blog/temporal-super-resolution-via-multisampling/),
built on his [spatial MSAA reconstruction](https://filmicworlds.com/blog/upsampling-via-multisampling/).
It replaces the previously proposed edge-AA/optional conventional-TAA stages;
do not copy v22's AA or stack another AA filter by default.

The article's configuration uses 4x MSAA, half-resolution input in each dimension,
output-resolution history, and a four-frame half-input-pixel jitter cycle.
Reconstruction reads individual samples, uses gradient-weighted spatial estimates,
then reprojects, clamps and blends history. This is temporal reconstruction, not
ordinary MSAA followed by a box resolve. Its example shares one source-pixel
motion vector across four reconstructed output buckets. Production robustness
and parameters still require validation for this game.

### Integration requirements for the selected method

The following are PPR integration requirements, not claims that the article
already solves these cases:

- Verify the pinned RHI supports the required multisampled HDR attachment format,
  sample count, sampled multisample views and per-sample shader loads on each
  supported backend. Verify actual sample positions/order against the resolve
  kernel. Unsupported capability is an explicit gate, not a silent AA substitution.
- Rasterize real contour coverage into the MSAA target. A compute shader writing
  one color per pixel, or an opaque cell quad with shader-only alpha smoothing,
  does not automatically provide distinct silhouette coverage samples. Establish
  geometry or explicit per-sample coverage consistent with contour rendering.
  Assess sample-frequency shading where required; avoid blindly quadrupling all
  material/lighting work. Preserve derivative-aware texture and normal filtering.
- Keep the MSAA color attachment unresolved until custom reconstruction consumes
  it. Define matching coverage/depth or surface-identity inputs for disocclusion.
  Motion must include previous/current camera translation and zoom plus actor
  motion, with explicit jitter conventions. Use current reconstruction whenever
  history is invalid, including out-of-bounds reprojection and first use.
- Current `GridFrame` intentionally carries an unjittered camera transform. Add
  jitter through owned display constants with matching inverse transforms and
  conservative culling margins. Preserve the authoritative camera and transport
  coordinates; do not change pipeline identity for each jitter phase.
- Parallax layers have different apparent motion. Refraction and transparent
  mixtures may not have one valid motion vector. Define separate layer resolve,
  validity masks or conservative history rejection for these cases; do not
  assume terrain camera motion describes the refracted backdrop.
- React to changed geometry, materials, emission and newly published lighting fields.
  Reject or reduce affected display history without resetting world transport
  for camera jitter. Define camera-cut, resize, world replacement and resource
  generation resets. Advance phase only with coherent accepted presentation work;
  track the jitter and history identity actually used by each submission.
- Prototype the source's 2x-per-axis reconstruction ratio first. Native-resolution
  AA, other ratios and dynamic resolution require separately derived mappings
  and quality checks; do not assume the same kernel is universal.
- Count all live MSAA attachments and history generations in resource admission.
  Measure total rasterization, motion-input and reconstruction costs, not just
  resolve time. Preserve existing fence-safe retirement on resize and failure.

### Runtime order and delivery stages

Extend the existing `RetroPass` resource owner for selected spatial/temporal
reconstruction, exposure, bloom and AgX. Preserve its fence-safe attachment/history
lifetime, independent palette toggle and native UI composition. Do not create a
parallel GridPass display owner. Current native-extent averaging is not the selected
2x-per-axis TSR; a 4K target needs the explicit 1920x1080-to-3840x2160 sample mapping,
coverage/identity/motion contracts and quality gates before claiming integration.


Use multisampled linear-HDR world composition, custom MSAA temporal reconstruction
to output resolution, exposure, HDR bloom, validated AgX Punchy mapping, exactly
one display encoding, then UI. Keep temporal history in a defined linear-HDR
domain and handle exposure conventions explicitly. The reconstruction-before-
bloom order preserves sample information until it is consumed. Background-only
refraction is part of world composition and must obey the motion/validity policy.

Deliver spatial sample reconstruction first as the temporal method's prerequisite
and current-frame path, then add reprojection/validity/history, then exposure and
bloom integration. Manual exposure precedes optional auto-exposure metering and
adaptation. These display additions are not claimed v22 ports. UI, gameplay
values and bounce feedback stay outside display processing. Test black,
negative/intermediate values, bright emission, saturation and the surface's
sRGB/linear contract. Fuse stages only after equivalence checks.

## Optimization order

Measure each optimization against the same unoptimized scene and generation:

1. Cache immutable quadrature/BRDF lookup resources.
2. Batch directions or tiles, alias proven nonoverlapping scratch and assess recomputation.
3. Reduce precision after HDR, long-path and near-opaque error comparisons.
4. Introduce complete dirty cascade dependency closures.
5. Represent provably empty/opaque intervals analytically while preserving exposed
   emission and required reconstruction dependencies.
6. Evaluate sparse directional storage around air/exposed surfaces while keeping
   globally valid scalar queries.
7. Add compaction/indirect dispatch and LDS/wave reductions where measured useful.
8. Prioritize visible changes while guaranteeing offscreen query progress.
9. Consider async compute only after the single-queue schedule is correct.

Use portable paths for supported backends. Chunk-local solves lacking external
transport are a changed model, not an equivalent optimization.

## Delivery and acceptance

| Milestone | Required result |
|---|---|
| 1. Contracts and references | Reuse implemented materials/matter, define optical/boundary contracts, CPU oracle and RC allocator estimator |
| 2. Acceleration | Scalar hierarchical DDA matches the oracle, including edits, bounds, transmission and incomplete-work status |
| 3. RC qualification | Fixed lattice, bilinear fix, FP32 oracle comparisons, stored-format tests and streamed full-world allocation admission |
| 4. Persistent global transport | RC passes global budgets; coherent generations, diffuse feedback and bounded scheduling |
| 5. Gameplay and maintenance | Publication/invalidation, minimized maintenance, asynchronous readback and age-aware queries |
| 6. Material shading and display | Directional diffuse/rough specular, HDR backdrop/refraction, spatial then temporal MSAA reconstruction and AgX/exposure/bloom |
| 7. Optimization | Paired correctness, memory and timing evidence for every retained change |

Fixtures cover opaque separators, one-cell doors/tunnels, diagonal contours,
chunk boundaries, distant offscreen emitters, colored absorption, emitter removal,
high reflectance, black materials, world replacement and continuous zoom.
Check angular/energy normalization, signed reconstruction, first-frame validity,
feedback convergence, stale results and closed-door history leakage.

Sweep emitter/probe phase through fractional offsets, including sub-probe
emitters and distant sources through tiny apertures. Before display processing,
measure blocked-side leakage, missed emitter energy, boundary displacement and
transition width against the linear-HDR oracle. Test tiny and extended emitters
to distinguish hard-shadow error from legitimate penumbrae. Transmission tests
must compare both `L` and `T` for identity intervals, ordered absorbing slabs,
colored and near-opaque media, emission behind glass and emission within a medium.
An outdated closed-door lighting field may be returned as stale, never as current.

AA comparisons include stationary convergence, slow/fast pan, continuous zoom,
subpixel contours and one-cell features, moving actors, changing illumination,
parallax, transparent/refraction boundaries, camera cuts and resize. Compare with
an offline high-resolution reference and a conventional MSAA resolve diagnostic.
Verify sample-index mapping, output alignment, border rejection and first-frame
behavior with synthetic patterns. Measure sharpness, ghosting, flicker and total
frame cost; source benchmark timings are not PPR performance promises.

Lifecycle tests cover partial initialization, submission failure, present failure
after accepted submission, busy rings, cancellation, minimized operation, resize,
reload, device loss and teardown. Extend applicable app/frame/GridPass and asset
GPU readback gates; gameplay cache/publication tests belong in game tests. Use
`PPR_TEST_ASSERT`, test behavior and error/lifetime contracts, not implementation
mirrors. Verify shader compilation, pipeline creation, bindings and execution on
supported backends.

Measure peak live bytes, largest resource, readback bandwidth, per-stage GPU
time, update latency and maximum query age on named hardware and representative
worlds. Agree on product thresholds before acceptance; do not borrow benchmark
numbers from external demonstrations as PPR promises.

Follow repository shader/module/build/validation skills during implementation:
host-specific serialized builds following `build-system`, affected
tests each milestone, the current registered matrix required by `validation` for cross-cutting rendering,
required repeated lifecycle/concurrency runs, formatting/semantic-style gates,
available inspections and independent review. Recheck current skill instructions
at execution time. Executable milestones require their affected build/test gates; historical documentation review does not establish implementation validation.

The provisional lighting memory/time, normal-speed update-age and physical-error
gates above were explicitly accepted by the user. GPU-less behavior and any
product relaxation needed if full-domain transport cannot meet those gates remain
open. MSAA temporal reconstruction is the selected AA method;
bloom and exposure extensions remain staged assumptions.
These decisions do not block the small-domain oracle or material-interface work;
they do block claims that the full product scope is accepted.

### Delivery guidance, 1 November 2026

At the user's request, Astra reviewed the next delivery steps. The coordinator
adopts this sequence: finish the bounded terrain RC gate, decide exterior contour
receivers and the explicit cut-face lighting approximation, then demonstrate a
bounded real-material scene through GridPass before expanding qualification
infrastructure. Include sediment, ore, an emissive boundary, a one-cell wall and
an opening/closing doorway. Unsupported media remain explicit; this milestone
does not admit ordinary full-world lighting.

Close the generic cell transport path's FTZ/underflow safety holes alongside this
work. Narrow numerical-domain rejection is an interim qualification policy;
production media require an explicit discarded-radiance error bound, including
bright continuation. Scene-level error thresholds do not establish that bound.

Evaluate compact material labels with the existing optical palette before
allocating dense production snapshots. Count overlapping generations and
publication resources. Separate compilation/loading latency from warm GPU work:
lazy pipeline creation has passed source review, but does not establish the
4 ms frame or one-second publication gates. Controlled HDR display fixtures can
progress independently; display grading must not conceal transport failures.

The lazy-family Dev build, cell-only readiness leaf and terrain trace-only
readiness leaf passed. The first terrain cascade schedule leaf failed at result
readback (`actual.has_value()`, `App.RadianceGpu.Tests.cpp:1198`); it did not time
out. Diagnosis is pending. Preserve
`out/diagnostics/environment-lighting-20261101/geometry/lazy-terrain-cascade-readiness.log`;
this result does not qualify terrain RC or the registered GPU shards.

The single instrumented repeat exposed existing diagnostic fields: row 8,
status 32 (`numeric_ambiguity`), zero local connector spans; the returned error
was `argument_out_of_domain`. Pipeline preparation/submission/fence completion
passed. Instrumentation was removed and the clean source hash restored; the
retained diagnostic binary is identified separately from current source.

For the independent manual-exposure/display batch, the coordinator selects
[Filament's scalar AgxToneMapper Punchy](https://github.com/google/filament/blob/fd9ec7ff9510df148011c5326bcb50d4bfb811ca/filament/src/ToneMapper.cpp)
at commit `fd9ec7ff9510df148011c5326bcb50d4bfb811ca` as an attributable AgX
approximation. This is not an exact v22 or OCIO-equivalence claim. Its linear
Rec.2020 contract requires explicit conversion from/to PPR linear sRGB.
Extend the owned `RetroSettings` snapshot, default off, with manual exposure
in the finite range [-16,+16] EV. Reject invalid settings before mutation;
keep history unexposed and apply exactly one display encoding for the actual
attachment format. Preserve upstream notices/license. Qualify the real pass
with independently generated upstream goldens and encoding/exposure invariants
before enabling this treatment in a game scene. This batch does not qualify
Hable reconstruction, bloom or full-world integration.

The exact-origin ownership correction passed independent shader source review;
its ordinary terrain trace leaf subsequently passed 87 cases, including mirrored
half-plane, zero-axis and translated/metric regressions. The combined Dev build
passed `engine.app`, `engine.tests.app`, `engine.tests.asset`, `game.tests` and
`app.game` after a routine HDR fixture API correction. HDR source review found no
functional blocker; its required test paragraph corrections were applied.
The ambiguity leaf also passed. The unchanged terrain cascade equivalence leaf
still failed at interval readback after 184.24 seconds; its numeric row fields
were not retained by the runner. Preserve `final-terrain-cascade.log`; no
successful cascade qualification is claimed. Test wall time includes shader
compilation and is not evidence for the 4 ms GPU budget.

The independent HDR CPU rejection, frozen-reference/encoding, rejected-policy
history and existing temporal/palette leaves passed. Both new HDR leaves also
passed with the system Khronos Core layer inserted at instance and device
creation. Those asset opt-in runs did not request GPU-assisted validation: the
entrypoint left the opt-in settings at RHI defaults. The earlier description of
that binary as ASan-off is unproven without retained compiler-command or symbol
evidence. The asset test entrypoint was subsequently corrected to set both
options explicitly from the opt-in. Both HDR leaves passed again; their native
`VALIDATION-SETTINGS` messages confirm GPU-assisted and Core validation enabled,
with the system Khronos layer inserted for both instance and device creation.
Preserve `hdr-picture-punchy-gpuav-fixed.log` and
`hdr-picture-rejected-policy-gpuav-fixed.log`. Windows/D3D12, the full registered
matrix and 4K cost/integration remain unverified; these bounded display checks
do not qualify terrain transport, TSR or bloom.

### Parametric terrain RC refinement

The coordinator adopts a single canonical internal parametric RC convention,
following independent design review. A cascade ray means `p + lambda*d_raw`,
where `d_raw` is the uploaded FP32 direction interpreted exactly and interval
limits are lambda values in fine cells. Its parent near-origin and each
connector target mean the same symbolic expression `E = p + nearCells*d_raw`.
Do not independently round an endpoint and reconstruct a different normalized
ray. Boundary connectors use `q + t*(E-q)`, with `t` in [0,1]. Public physical
metre ray queries retain their existing normalization contract through a wrapper.

This is a deliberate numerical schedule refinement, not bit-for-bit equivalence
with the earlier metre-based RC implementation. Compute medium length as
`deltaLambda*length(d_raw)*cellMetres`, or `deltaT*length(E-q)*cellMetres` for
connectors. Certify finite/underflow behavior and terminal boundary reach under
this convention. The small radial variation from quantized direction length
must pass the existing physical holdouts; parent support, angular weights,
connector-before-weight composition and quality gates remain unchanged. The
independent Bezier oracle must consume this declared schedule without sharing
the GPU intersection implementation.

Boundary receivers retain a validated same-cohort curve witness and symbolic
midpoint for arbitrary authored roundness; no artwork rounding or exact-float
material subset is adopted. Only the witnessed zero contact is algebraically
excluded. Occupancy, later positive roots and source ownership must be certified
for the complete occupied region. Inward bent connectors retain immediate
opaque source/T=0 semantics, without omitted or renormalized dependencies.
Only proven identical endpoints yield identity transport. Implementation and
qualification of this refinement remain pending.

The immediate generic-cell FTZ safety batch deliberately narrows its qualifier
domain: raw FP32 subnormals and unpreservable positive intermediates return
visible errors. Existing sigma .25/32 long-corridor queries remain unchanged but
must now assert that domain failure in both storage modes; sigma 2^-12 success
and half-error comparisons remain. This closes silent loss and does not qualify
production media. A bounded discarded-radiance policy is still required before
ordinary water/absorbing-world admission.

### Current source and validation follow-ups, 2 November 2026 (Europe/Paris)

Independent review of the parametric source found two issues before execution:
the GPU source query discarded its hit enclosure, and the new CPU reference
could return an infinite physical hit distance from finite lambda inputs.
Both fixes are implemented and targeted source review is closed. The GPU keeps
the full earliest-hit enclosure and makes its source certificate explicit;
the CPU guards physical-distance overflow and positive-distance collapse.
The immediate source certificate permits only proved common-source, singleton-point
or certified straight-partition decisions; uncertain differing sources remain
numeric ambiguity. This is an intermediate qualifier, not complete curved
multi-material source support. General source certification remains required
before ordinary-world/emitter admission; existing fixtures and source rules
must not be changed to manufacture success.

The registered `engine.tests.app.functional_cpu` entry passed all 220 leaves in
79.10 seconds, including all 19 lighting leaves. The unchanged terrain cascade
entry still failed after 127.22 seconds: row 28, status 32 (`numeric_ambiguity`),
12 segments, with `argument_out_of_domain` at readback. The tested app binary is
`cb0ea334dfbbb6e130d5dd73253c41059d7407787f28bc83f217b5b335c813ed`;
retain `parametric-terrain/validation-summary.json` and its linked source,
staged-shader and execution artifacts under
`out/diagnostics/environment-lighting-20261101/`. The approved high-bit status
diagnosis subsequently localized inherited ORDER ambiguity to the root-versus-span
guard in a local connector of C1 record 57. Earlier failure evidence remains
applicable history. No ordinary-world admission follows from the CPU pass.

The bounded stable-quadratic correction passed targeted independent source
review and the unchanged registered terrain cascade fixture: 122.57 seconds for
the leaf, 124.61 seconds for CTest, process exit 0. It replaces cancellation-prone
quadratic numerators with ordered `q/a, c/q` enclosures using the existing
outward-rounded primitives and unchanged domain, separation and event guards.
An FP32-input reconstruction demonstrates a root near 0.373295 safely inside
its cell span; its former enclosure crossed both endpoints. This reconstruction
supports the remedy without claiming runtime coefficient capture or a unique
connector observation. The tested binary is
`ac72ffab551ec9755981a1799c130330555ef9c675db21b8f763d7b9a743ef0e`;
retain `parametric-terrain/stable-q/final-provenance.sha256`, the proof/replay
artifacts and `cascade-run.log`. The registered ordinary trace subsequently
passed 89 cases (86.18 seconds), and the ambiguity/collinear leaf passed
(14.89 seconds). All three exact linked-component exports pass SPIR-V validation;
every direct FP add/subtract/multiply/divide result carries `NoContraction`,
with no `RelaxedPrecision` or `FPFastMathMode` decorations. Retain
`stable-q/ir-analysis.txt` for commands, hashes and inspection scope.
The registered cascade Core/GPUAV follow-up timed out at its unchanged
300-second deadline without a result. Both validation modes were confirmed
active. A late process snapshot showed CPU activity, which does not identify
the responsible subsystem. Retain `stable-q/cascade-gpuav-run.log` and its
process snapshot. The one approved follow-up with the existing diagnostic
enabled passed all assertions/readbacks with Core and GPU-assisted validation
active: leaf 294.35 seconds, CTest 296.92 seconds, process exit 0. Preserve
`stable-q/cascade-gpuav-diagnostic/summary.txt` and its raw evidence. Producer
timestamps bound 289.04 seconds between the rejected budget probe and first
post-submit fence probe, spanning valid preparation, lazy pipeline creation,
encoding, command-buffer finish and queue submission. They do not isolate an
individual call or measure GPU execution. The earlier timeout remains retained
and the successful run has little margin under the unchanged 300-second cap.
Shader compile time is included in these wall times; no warm GPU budget is
established.

Following the next requested Astra guidance, proceed to the bounded lit GridPass
scene while closing GPUAV qualification in parallel. Use exterior witnessed
receivers and the declared within-component cut-face continuation. The first
visible diffuse stage fixes the front normal to `(0,0,1)`: under the proposed
uniform-elevation lift its kernel is exactly one half, so reflected color is
albedo times angular mean incoming radiance, plus separate self-emission.
Normal-map lighting and rough specular remain required subsequent work; no
general-normal kernel is admitted by this first stage. Mixed-roundness geometry
uses a declared canonical binary32 RTE mean of authored endpoint values.
Current linked SPIR-V does not select a rounding mode, and `PRECISE` alone
does not establish that contract. Implement one shared integer-bit construction
for raster and terrain shader coefficients and construct/promote that same
coefficient on CPU before independent Bezier intersection. Keep authored
endpoints unchanged and retain explicit numerical-domain failures. Exact-bit
cross-path qualification remains pending; a matching-looking FP expression
does not establish shared geometry.
One follow-up GPUAV run enables the existing producer diagnostic at the unchanged
300-second cap; it is an observation run, not a waived validation gate.

The current `clang-dev` qualification uses effective ASan and UBSan enabled:
developer mode sets both in normal CMake scope despite an ASan-OFF cache entry,
and generated compile commands contain both sanitizers. Cache values alone do
not establish effective policy. The explicit Core/GPUAV activation messages
retained for the HDR leaves remain valid independently of this correction.

The first Hable prerequisite is a standalone signed linear-HDR spatial kernel
and raster sampling/filter fixtures, not RetroPass activation. It uses the
later article's continuous diamond helper and deliberately corrects the
downloaded source's repeated lower output taps to four distinct grid points.
The retained MIT source, SHA-256, coordinate derivation and independent
constant/affine invariants are in `third_party/HableMSAA/README.md`. The native
threshold probe gives finite coordinate brackets and an index mapping for
tested pixels; it is not proof of an exact/global sample pattern. Runtime
admission, real contour sample coverage, temporal history and 4K cost remain
open. The four source-review paragraph findings and two routine fixture API
compile errors were fixed; targeted review is closed. Both ordinary spatial
leaves and the registered spatial Core/GPUAV entry passed on the RTX 2080.
Evidence is under `hable/compile-fix/`, asset binary
`28dd386d343f887468477a8b7addfb120d9158f4f6a38ba0d6fbcfe0bc52475f`.
The independent native property read additionally reports
`standardSampleLocations=true` and device-wide 4x color/depth sample support;
retain `hable/hardware/rtx2080-properties.json`. Runtime engine admission and
format-specific checks are still required before RetroPass activation.

The Vulkan admission recipe's first configure failed closed on source identity:
its original hashes came from a clean older CPM checkout, not the selected
`a78e208be303df9f934a52e5557665b9b3538cb9` pin. No overlay was generated by
that failed configure. The corrected recipe checks the actual pin's public
header, Vulkan device implementation and unchanged shared native image builder
against these normalized LF SHA-256 hashes, respectively:
`31bd6ad07b48543a1026b2abab5ba582781d691cfdf9394b3a6a2013e47a69eb`,
`274541a3e60b84913c24358401a9a09d718946b382ad98c1e47f8a34093a457d`,
and `10797bb78d5444f804982e5e7c9f0d92befc28190c19dc6bfa1dc826b3cb6a9d`.
Its bounded allocation guard requires a null heap and zero heap offset, and
queries the exact normalized native builder fields for committed creation
without initial data; placed resources remain outside this scope. The correction
passed targeted source review; corrected configure passed twice (1.8/1.7 seconds)
with twelve idempotent overlay files, eleven upstream-identical sibling headers,
no original public include root, overlay system includes for the patched unit
and 182 project entries, and a clean unchanged CPM checkout. The corrected
app and asset builds completed after routine public-result-type correction.
The registered spatial Core/GPUAV entry subsequently passed both leaves and
process teardown in 7.01 seconds, including the focused allocation admission
checks; native messages confirm both validation modes. Preserve
`rhi-msaa-integration/spatial_gpu_validation.log`. Earlier standalone launches
had incorrect working-directory/runtime-environment setup and are not clean
ordinary post-recipe gates. No new sanitizer suppression was introduced.
Project engine/test compile and link commands enable ASan and UBSan; the external
patched Slang-RHI translation unit is unsanitized.
Retain
`rhi-msaa-integration/configure-first.log` and the source-identity evidence in
that directory; provenance is in `third_party/SlangRHI/README.md`. This changes
neither the dependency version nor ordinary-game spatial activation. D3D12
standard-quality coherence remains required follow-up work.

The next default-off spatial integration is in progress: an independently
queried sample-rate-shading capability, actual sample-frequency opaque terrain
and background, then RetroPass's fenced 2x reconstruction before grading and
native UI. Standard sample locations alone do not establish sample-rate-shading
admission. Initial strict policy rejects legacy temporal/pixelization combinations,
odd, nonpositive or nonexact 2x extents and unsupported particle/translucent/refraction content;
the conservative opaque-only visual inventory is an initial milestone restriction,
not full-world admission. Actual GridPass per-index readback and real RetroPass
reference/lifecycle checks remain required. Newly authored spatial code is
unreviewed and unqualified; its presence does not establish activation or TSR.

Measured isolated radiance leaf costs could not fit the previous registered
GPU aggregate deadlines. Registration now uses six disjoint radiance shards
under each ordinary/GPUAV policy, preserving all twelve leaves, GPU locks,
loop settings and 300-second per-shard deadlines. Grid/pixel coverage remains
separate. Two measured slow asset leaves have their own 300-second ordinary
shards; the residual asset scope retains 450 seconds and its timing is still
unverified. Dedicated HDR and spatial GPUAV entries select their respective
two leaves. Configure and metadata audits passed with fifty entries and
selector lengths at most 304 characters; this proves registration, not suite
execution. Independent registration source review passed. Evidence:
`registration-preflight-final/registration-audit.json`.
