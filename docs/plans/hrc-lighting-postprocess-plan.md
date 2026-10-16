# World-space lighting and post-processing implementation plan

Status: proposed for review, revised 13 October 2026. Prepared with GPT-6 Astra and
reviewed against current repository sources. Updated with the user's selected
MSAA temporal reconstruction method and a single-solver RC implementation scope.
No lighting implementation or builds have started. Names for new components
below are proposals, not existing APIs.

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

Implement **accelerated world-space Radiance Cascades (RC)**, reusing the
hierarchical visibility/jump-table design from v22. HRC is outside this
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

### Accelerated DDA contract

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
GPU/app types. Agree on acceptable age before full gameplay acceptance.

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
serialized builds in the prescribed persistent Windows toolchain shell, affected
tests each milestone, the full twelve-entry matrix for cross-cutting rendering,
required repeated lifecycle/concurrency runs, formatting/semantic-style gates,
available inspections and independent review. Recheck current skill instructions
at execution time. This documentation-only delivery needs factual/link review,
not executable builds.

Outstanding decisions are target GPU memory/time budgets, accepted gameplay
update age, GPU-less behavior, and any product relaxation needed if full-domain
transport is unaffordable. MSAA temporal reconstruction is the selected AA method;
bloom and exposure extensions remain staged assumptions.
These decisions do not block the small-domain oracle or material-interface work;
they do block claims that the full product scope is accepted.
