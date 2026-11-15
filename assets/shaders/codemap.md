# assets/shaders/

`pbmpm/solver.slang` and `pbmpm/types.slang` are the direct pinned PB-MPM
compute port owned by `engine.matter`. Their source attribution, six-entry-point
schedule plus publication validator, explicit scalar CPU/GPU ABI and admitted scope are recorded in
`pbmpm/README.md`; `pbmpm/LICENSE.md` retains the upstream BSD-3-Clause notice.

`pbmpm/terrain_motion.slang` is the production terrain adapter: sample-centred
grid coordinates, same-substep point recovery, canonical-segment sweeps and
local convex/concave vertex response. It separates geometry from retained
motion and constitutive state, certifies published positions, and rejects
unresolved/incomplete paths through the existing transaction error buffer.
Its terrain-local rounded-leg helper protects two FP32 operations on the pinned
Slang compiler; source/core arithmetic and resource layouts are unchanged.

Production Sand additionally retains a unilateral objective-volume affine impulse
and uses terrain Coulomb friction0.5 at grid and swept endpoint contacts. The
legacy volume flag gates the impulse for Sand as well as liquid. Sand gathers
its separate contact displacement for both translation and APIC; liquids and
other materials retain the base contact path. The extra shared tile is512 bytes
per64-thread group, with no new device allocation or ABI. See the shader README
for the fixed K2 gain, contact ordering and grid-scale density limitations.

## Responsibility

Slang shader assets for application render passes. `mesh_bindless.slang` is the
`TrianglePass` static-mesh program; `grid_tiles.slang` is the `GridPass` debug-grid program (flat
tinted or per-cell quads, one instance per visible chunk-sized tile). `post_process.slang` and
`present_scene.slang` supply the independent resolved-size treatment and fixed-Presentation
enlargement through `PostProcessPass`. The former `triangle.slang`
reference asset was deleted. Each pass owns its shader program, and diagnostic
fixtures load their qualification assets directly.

`grid_visibility.slang` supplies GridPass compute reset and cull entries. It
matches the CPU reference's conservative boxes and frustum planes, atomically
appends at most 1024 visible IDs, and writes one indirect draw record. The tile
vertex entry optionally resolves instances through those IDs. Reset, cull,
barriers and drawing share one encoder and submission.

`tests/world_lighting_backend.slang` is a test-only qualification asset for the
`app/world_lighting_backend/` group. It declares the R8_UINT jump UAV/SRV bound
as `uint` with an explicit `r8ui` image format, uint32 winner scratch reductions,
the 4/8/32-byte structured records, the four-sample target dump entries and the
history/status probes. No runtime pass loads it.

`world_lighting_codec.slang` is the shared explicit-word mirror of the C++
world-lighting records (`PackedInterval`, `PackedCH`, `SourceMaterial`, resolved
cell). It decodes FP16, RGB9E5 and signed int8 AC bytes with integer word
extraction only; no native half3/int8 struct layout is part of the ABI.
`thermal_emission.slang` owns the one `StructuredBuffer<float4>` thermal table
binding and `thermalRGB(float kelvin)` (log-spaced row interpolation, zero below
256 K, no per-temperature normalization). Both are include-only fragments.
`tests/world_lighting_codec_probe.slang` and `tests/thermal_emission_probe.slang`
are test-only entry-point wrappers for `app/world_lighting_codec/`.

`world_lighting_source.slang` prepares the cached outgoing-radiance source
(`prepareSource`, 16x16 threads over the 1024-square domain): one RGB9E5 word
per native cell from the derived eight-byte material palette, the committed
thermal table and the prior fine-CH DC field, sampled bilinearly at
`(cell+0.5)/2-0.5` with clamped edges and scaled by `Y0`. `world_lighting_sky.slang`
is the include-only analytic terminal sky (SkyColor 0.2/0.5/1, SunColor 20/10/2,
SunA 5, SunS 64, fixed intensity 5, narrow unwrapped sector integration split
once at the 2*pi seam, negative hour = black boundary sky).
`world_lighting_cascade.slang` owns the two cascade entries:
`updateTerminal` (L5, four terminal sub-sectors per stored direction with
`D=16,384` traced to the actual map exit and closed by the sky with zero
transmittance) and `updateCascade` (L4..L1 near segment plus four retained
parent-sector merges with bilinear spatial weights). The fixed four-frame
quarter-probe schedule, probe geometry, derived trace directions and interval
atlas addressing are shared with fine reconstruction through
`world_lighting_schedule.slang` (include-only).

`world_lighting_inspect.slang` is the retained C-stage inspection overlay
(vertex `vertexInspect`, fragment `fragmentInspect`). It reconstructs the world
plane from the captured inverse view-projection and decodes only published data:
fine-CH angular mean with the two-cell probe lattice, cached source RGB,
occupancy classification, the stored jump classes, or the selected L1..L5
interval atlas (radiance left, transmittance right). Radiance uses the fixed
`max(RGB,0)/(1+max(RGB,0))` inspection mapping; categorical colors are literal.
It writes no state and is drawn after post-presentation and before native UI.

`world_lighting_projection.slang` is the `WorldLightingPass` categorical
projection: `clearWinners`, `reduceKeys`, `reduceRows` and `resolveCells` share
one native-cell coordinate/key helper over the 1024-square domain. It reads the
committed hot64/metadata rows and the 96-byte visible palette, atomic-mins
class/distance keys and stable-ID metadata ranks into two uint32 scratch grids,
and overwrites every resolved cell with terrain, the selected particle's packed
material/temperature word, or zero. `numeric_status.slang` defines the bounded
32-byte numeric status record (invalid-state/range-error atomic minima, reserved
zero words) shared by every numeric-producing entry.
`tests/world_lighting_projection_bind.slang` is the test-only reader for the
`bind` resolved-cell/status contract.

`world_lighting_visibility.slang` owns the two full-domain acceleration entries.
`buildVisibility` (16x16 threads, 32x32 groups) tile-reduces one 32x32 native
tile per group with 160 bytes of shared storage and writes the three levels of a
single 21,504-record uint2 buffer; `buildJump` (16x16 threads, 64x64 groups)
selects the largest aligned empty square per native cell and writes one R8_UINT
texel. `world_lighting_trace.slang` is the shared include-only segment tracer
with slab clipping, aligned-block jump advance, resolved classification and the
cached per-cell source; `traceSegment(start, end)` is the common helper and
`traceSegmentDetailed` carries the owning invocation and status record.
A nonfinite face parameter is a reported numeric failure rather than a clear
interval, and `traceAdvanceAxis` derives the entered cell from the original ray
faces so a corner or aligned-block-boundary crossing always keeps its next face
strictly ahead.
`tests/world_lighting_trace_probe.slang` is the test-only batch probe and
`tests/world_lighting_cascade_probe.slang` exposes the real cascade schedule
(phase, selected probe, derived age/actual trace frame, phase fraction and
direction) for the `app/world_lighting_cascade/` group. The
`world_lighting_codec.slang` mirror also owns the 96-byte `TerrainVisual`
classification row and `resolvedCellOccupied`, shared by projection,
acceleration and tracing, plus the explicit binary16/RGB9E5/interval/CH
encoders (`packHalf`, `packRGB9E5`, `intervalPackCategory`, `packInterval`,
`chPackCategory`, `packCH`) that mirror the host codecs for the producer
stages.

`world_lighting_ch.slang` is the include-only orthonormal circular-harmonic
basis, weighted sample fit, `angularMean(CH)` and world-cell
`angularMean(float2)` helper. The including module declares its fine-CH
binding before the include (a structured read for visible/gas readers, a
read/write field for the in-place writer). `world_lighting_fine.slang` owns
`reconstructFine` (8x8 threads, 512-square): four clamped L1 taps, the 64
nominal connectors, the near/far merge, exact weighted fitting, the in-place
`1-exp(-2*dt)` EMA and one encode per record. It reads only its own prior
record and writes all 32 bytes. `tests/world_lighting_fine_probe.slang`
exposes the real fit/EMA helpers, the retained L1 schedule identity and
`angularMean` for the `app/world_lighting_fine/` group.


## Design

`grid_particle_prepare.slang` is GridPass-owned resident-particle presentation:
numerical64/readonly24/metadata32/packed24/frame128 byte protocols. Packing uses
the committed hot/cold row identity and physical metric; three stable visibility
scan/compaction dispatches produce ordered IDs and indirect arguments without
reordering solver storage. The packed row's fourth word is the stable-identity
material-UV seed; the palette tint is decoded from the row's own element at
shading time. `grid_tiles.slang` shares the particle quad helper.
The only remaining particle draw is the sampled disk for gases and explicit
legacy phase fixtures; Task 11 retired the smooth solid/liquid splat, filter and
composite entries together with `grid_liquid_kernel.slang`,
`grid_solid_kernel.slang` and `grid_fluid_filter.slang`. Solids and liquids are
shaded through the world-lighting resolved-cell terrain path instead.

`world_lighting_material.slang` is the include-only visible response:
`diffuseIrradiance(CH, float3)` and `specularRadiance(CH, float3, float)` implement
the approved circular filters, with continuous zero-XY pole limits, and
`visibleMaterialResponse` composes kD/kS, the sampled BRDF pair, authored and
thermal emission. `material_brdf_lut.slang` owns the one-time 64x128 RG16F
isotropic VNDF/Smith integration entry (`integrateBrdfLut`, 1,024 samples/texel,
alpha=max(r,0.001)) and the bilinear/clamp pair description. GridPass retains the
resulting immutable texture.

- `mesh_bindless.slang` uses PPR's row-major, row-vector convention: transforms call
  `mul(vector, matrix)`, never `mul(matrix, vector)`. View-projection remains `view * projection`.
- Pure `StructuredBuffer` fetch driven by `SV_VertexID`: no fixed-function geometry bindings, so the
  pass needs no input layout.
- Descriptor-backed `ConstantBuffer<FrameConstants> g_frame` (unchanged288B)
  and `ConstantBuffer<DrawConstants> g_draw` (16B/four scalar u32 fields) accompany
  the structured vertex/index/material/payload buffers. Root ordinary data stays
  absent, preserving the D3D12 globalParams/g_frame binding contract.
- `vertexIndirectMain` has no ordinary entry parameters. The group's first
  payload index is `g_draw.m_payload_base`; C++ writes the cached dereferenced
  per-slot draw cursor. `SV_InstanceID` remains draw-local and
  `startInstanceLocation` remains zero on every backend.
- Textures are one bindless heap, `StructuredBuffer<DescriptorHandle<Texture2D<float4>>> g_textures`,
  passed as a fragment entry-point `uniform` parameter alongside a single shared
  `SamplerState.Handle g_sampler`. Heap slot 0 is the reserved white fallback, `kNoTexture`
  (`0xFFFFFFFF`) resolves to it, and albedo/ORM/emissive MULTIPLY the factor values.
- Chain: `nointerpolation uint material` -> `nonuniform(g_materials[input.material])` ->
  `m_textures` -> `resolveTextureSlot` -> `nonuniform(g_textures[slot])`. The divergent material
  index needs `nonuniform(...)` because it indexes a classic SRV, not a descriptor heap.
- Opaque/mask material shading only, with ORM channel mapping (R occlusion, G roughness,
  B metallic). Blend is rejected at pack time.
- Entry points: `[shader("vertex")] vertexIndirectMain` (the only vertex entry) and
  `[shader("fragment")] fragmentMain`. The name is `vertexIndirectMain`, not `vertexMain`; the C++
  looks it up by string.
- `grid_tiles.slang` shares the row-major, row-vector convention (`mul(world, m_view_projection)`,
  `view * projection`, `[0,1]` depth, no transpose/Y flip) on the z = 0 plane. Its
  `ConstantBuffer<GridFrame> g_grid_frame` carries the raster matrix (optionally jittered), visibility
  lookup flag, cell metric, surface/diagnostic mode and boundary material ID.
- `grid_tiles.slang` draws six non-indexed verts (two triangles) per tile rect from
  `StructuredBuffer<GridTilePayload> g_tiles` (CPU-mirrored 32 B: rect + material + cell-word base +
  flags + pad); rasterizer culling is off. Payload tables begin at zero; the
  vertex entry has no ordinary-data parameter. GPU draws resolve compact visible
  IDs, and reference indirect draws use their instance offsets. Removing the
  former always-zero tile-base parameter avoids a second entry-point push-constant
  range on Vulkan alongside the fragment sampler handle.
- `grid_tiles.slang` categorical lane: with published resolved cells, each raster sample
  reconstructs its own world-plane position and selects exactly one native packed
  cell (`material` in bits 0-15, FP16 Kelvin in 16-31). Empty samples discard, so
  MSAA internal boundaries receive real per-sample coverage instead of one
  pixel-centre ID. The material path samples linear texture RGB times linear tint,
  authored normal/metallic/roughness/IOR (publication-validated), `V=-forward`
  for orthographic or `normalize(origin-sample)` for perspective,
  `diffuseIrradiance`/`specularRadiance`, the shared BRDF pair and one
  authored+thermal term. Continuous world-UV gradients are captured before cell
  selection, and the categorical branch returns before the retired contour /
  cell-word / material work. The older packed chunk-cell lane remains only as a
  temporary fallback for worlds with no published resolved cells; Task 14/15
  owns its removal.
- Catalog coverage follows the occupied-vs-empty marching-square contour, with
  material-specific quadratic Bezier arcs for diagonal cuts. Roundness 0 uses
  the straight chord; 1 places the control point at the dual-cell centre.
  Each outer arc averages the solid materials at its crossed-edge endpoints.
  Repeated diagonal solids connect; distinct diagonal solids separated by empty
  corners retain disconnected occupancy. Collision geometry remains unchanged.
- Materials partition the occupied region by maximum signed contour distance,
  filling junction gaps even when no independent label shape contains the pixel.
  Two-solid dual cells use shared mean roundness. Competing diagonal solids
  retain the lower-ID bridge policy in their label scores.
- Terrain presentation draws one centered two-pixel internal material border
  (one pixel per side), and a two-pixel inward rim on the exterior solid border.
  Both have a one-pixel AA transition and fade as full nominal width grows
  from 0.2 to 0.3 fine cells. Exterior rims and shadows use occupancy curves,
  preventing phantom borders around the old independent material regions.
  A twelve-pixel shadow falls into non-solid space, capped at one cell.
  Derivatives captured before divergent branches convert normals to screen-space
  widths. A bounded 3x3 neighboring dual-cell search carries exterior effects
  across cell/chunk seams; curve bounding boxes reject unnecessary solves.
  Internal border distance uses signed-score differences and their projected
  analytic gradients. Diagnostics retain their exact palette colors.
- Boundary noise uses one shared texture and four explicit mip samples per
  catalog fragment, combining persistent macro and fine-detail bands. Material roundness blends angular texel ramps with broad plateaus
  into smooth quintic interpolation. A shared cell style and harmonic-mean scalar C1 envelope
  prevent material interfaces and adjacent dual cells from warping apart.
  Draw counts and resource bindings stay unchanged; curve evaluation and
  neighboring terrain reads add bounded fragment work.
- `grid_tiles.slang` entry points: `[shader("vertex")] vertexGridMain` and
  `[shader("fragment")] fragmentGridMain`.
- `post_process.slang` consumes the resolved scene view and bloom level 0 at the
  resolved extent and writes display-linear RGBA16F. Its 32-byte
  `ConstantBuffer<PostProcessParameters>` carries the resolved width/height, bloom
  intensity, manual EV, bloom-enabled flag and reserved words. `g_linear` combines
  the bloom tap; `g_exposure` supplies the 1x1 adapted offset. `displayAgxPunchy`
  runs exactly once on scene + `intensity*bloom` with total
  `exp2(manual_ev + adapted)` exposure. Entries: `vertexPostProcess`,
  `fragmentPostProcess`.
- `present_scene.slang` is the fixed-P nearest enlargement/crop: each window pixel
  loads `floor(pixel/P)` from the resolved output and the R*P-W excess is cropped
  rather than stretched. Its 16-byte constant block carries resolved extents,
  the pixel scale and the single SDR sRGB encoding decision. Float targets stay
  linear; sRGB attachments encode in hardware. Entries: `vertexPresentScene`,
  `fragmentPresentScene`.
- `luminance_histogram.slang` downsamples the valid R region to min(R,256) with
  integer partition blocks, builds the 64-bin [-8,8] log2 histogram and reduces
  a fractional 10-90% percentile mean. `auto_exposure.slang` applies the
  exponential adaptation with finite EV clamps.
- `display_agx.slang` is the attributed Filament Punchy approximation invoked by
  `post_process.slang` and never by reconstruction/history/presentation.
- CPU mirrors and cursor bindings live in `TrianglePass`, `GpuCaches`, and `Mesh.Types`.
- The ImGui shader remains embedded in `App.UI.ImGui.cpp`, not under this asset directory.
- CMake stages the whole shader directory beside application and relevant test executables, so
  adding or deleting a `.slang` file needs no build-system change.

## Flow

`TrianglePass::initialize` -> `IShaderService::loadModuleFromFile(<content_dir>/shaders/mesh_bindless.slang)`
-> `io::mapFile` -> `MappedFileBlob` -> `ISession::loadModuleFromSource` -> `vertexIndirectMain` +
`fragmentMain` linked into the pass-owned program and pipeline variants; `ShaderCursor` binds frame
data, the structured buffers, the `g_draw` constants per draw group, and the descriptor heap before
each draw. `GridPass::initialize` follows the same chain for `<content_dir>/shaders/grid_tiles.slang`
(`vertexGridMain` + `fragmentGridMain`), binding frame constants, tile/cell and
contour/diagnostic buffers, and the bindless texture heap/shared sampler. `PostProcessPass::initialize`
loads `luminance_histogram.slang`, `auto_exposure.slang`, `spd_bloom.slang`,
`post_process.slang` and `present_scene.slang`, then binds the resolved scene,
adapted exposure and bloom level 0 before the single AgX draw. Offscreen output,
bloom and metering cohorts are reused only through accepted-submit fences.

## Integration

- **Consumers**: `engine.shader` (compile), `engine.rhi` + `TrianglePass`/`GridPass`/`PostProcessPass`
  (pipelines + constants), `game` demo.
- **Depends on**: Slang compiler; `engine.math` matrix-layout/handedness conventions (row-major,
  row-vector, left-handed +Z forward, [0,1] depth).
- **Provides**: the runtime bindless mesh shader asset for the scene pass, the grid-tile shader
  asset for the debug-grid pass, and the resolved-size post/presentation shader assets.
- **Known limitations (documented in-shader, deliberately unfixed)**: the model matrix must be a
  rigid transform or a uniform scale, since directions are rotated with `mul()` rather than the
  inverse-transpose; and `FrameConstants` carries four fields (`m_view`, `m_projection`,
  `m_inverse_view_projection`, `m_viewport_size`) that this shader never reads but that the CPU
  `sizeof == 288` assertion and `uploadFrameConstants_` still require.

## Key Files

- `mesh_bindless.slang` - the live bindless static-mesh vertex/fragment program.
- `grid_tiles.slang` - the live debug-grid tile vertex/fragment program (`vertexGridMain` + `fragmentGridMain`)
  and the particle lanes: direct gas/solid classes plus the surface reconstruction's minimum-presence marker
  (`vertexParticleSurfaceMarker` + `fragmentParticleSurfaceMarker`), label selection
  (`fragmentParticleSurfaceSelect`), attribute accumulation (`fragmentParticleSurfaceAccumulate`) and
  per-scene-sample deferred composite (`fragmentParticleSurfaceComposite`).
- `grid_particle_density.slang` - include-only screen-space particle-surface mathematics: the revision-5 shared
  radius (R = 1.525948559 r), A_2 = 10/(7 pi) normalized anisotropic cubic sigmoid, retained 5/28 threshold,
  squared-radius selection metric, binary body occupancy (B = [D > theta], no per-sample ramp) and the
  revision-8 minimum-presence marker (F = saturate((R - d) / (R - core)) unioned as max(B, F), radius in raster
  samples = 2 content pixels); constants and pure functions only, consumed by `grid_tiles.slang`.
- `world_lighting_material.slang` - include-only visible CH diffuse/specular filters and material response.
- `material_brdf_lut.slang` - one-time 64x128 RG16F isotropic VNDF/Smith integration entry and sampling helper.
- `world_lighting_source.slang` - cached-source preparation over the full native-cell domain.
- `world_lighting_cascade.slang` - scheduled coarse cascade transport (`updateTerminal`, `updateCascade`).
- `world_lighting_schedule.slang` - include-only four-frame probe schedule, probe geometry, directions and interval atlas.
- `world_lighting_ch.slang` - include-only CH basis/fit and angular-mean helper.
- `world_lighting_fine.slang` - in-place fine CH reconstruction and EMA (`reconstructFine`).
- `world_lighting_sky.slang` - analytic terminal sky sector integration (include-only).
- `post_process.slang` - resolved-size bloom combine and once-only AgX treatment.
- `present_scene.slang` - fixed-P nearest enlargement/crop and single sRGB decision.
- `luminance_histogram.slang` / `auto_exposure.slang` - metering and adaptation.
- `display_agx.slang` - display treatment included by `post_process.slang`:
  attributed Filament scalar Punchy approximation, explicit linear-sRGB/Rec.2020
  conversions and final SDR gamut clipping. See `third_party/FilamentAgX/README.md`.
- `spatial_msaa4.slang` - standalone signed linear-HDR MSAA4-to-2x spatial
  reconstruction prerequisite, currently loaded by asset fixtures. It uses an
  admitted sample-index mapping, continuous diamond weights and corrected
  lower-bucket sums; source/derivation are in `third_party/HableMSAA/README.md`.
  The raster threshold probe establishes finite coordinate brackets for tested
  pixels, not an exact/global native pattern guarantee. Runtime pattern
  admission and real terrain sample coverage were integrated in Tasks 11–14;
  the standalone shader remains the qualification fixture.
- `hable_temporal.slang` - always-on temporal Hable reconstruction
  (`reconstructTemporal`): quarter-pixel lattice, continuous diamond weights,
  contributing-original-sample bounds, reprojected history rejection and the
  5%-25% current-frame blend. Scene-linear/unexposed; history reset/reprojection
  policy lives in `ReconstructionPass`, not in this shader.
- `tests/reconstruction_probe.slang` - test-only per-sample scene writer and
  MSAA sample dump used by the reconstruction lattice/oracle agreement tests.

Catalog terrain uploads the shared `engine.math` contour points and 16-case
connectivity table verbatim (8-byte points, 72-byte cases). The fragment shader
reads global samples through a completion-safe logical-chunk-to-physical-slot
map, including diagonal neighbors, then rounds the shared midpoint boundaries.
The shader complements the opposing saddle case for the selected bridge
material; physics retains the original disconnected saddle table. A pass-owned
64-square linear RG noise texture has seven mip levels. Fixed world-space macro
and detail bands use texture periods of 512 and 16 fine cells, with gains 0.075
and 0.011 independent of zoom and roundness. Each band smoothly blends adjacent
snapped mip levels at identical UVs; zoom filters fixed features rather than
morphing spatial octaves. A conservative footprint covers both interpolation
profiles, including sharp plateaus. The combined displacement is bounded by
0.086 cells per axis. A harmonic-mean scalar C1 envelope fixes both components
on every dual-cell edge and fades when cells become unresolved. The mean solid-
corner roundness controls angular versus quintic interpolation only. The diagnostic sample
comes from the chosen material with global row-major ties. Outside-world samples
use the profile's validated wall material without forming atlas addresses. Each
chunk draws only its nominal rectangle. Explicit legacy fixtures retain square
cells. Catalog color textures use world-space metric UVs and explicit gradients.

The background program receives exact generation heights for all 4096 fine
columns in a separate 16 KiB constant buffer. The existing 304-byte frame
carries its enable flag and cell metric. Above each column's surface it draws
a sky gradient; below it retains the four authored parallax layers. The same
rendered backdrop supplies optical terrain and particle refraction.

## Selected-fork conformance fixture

`pbmpm/fork_conformance.slang` is test-only native conformance for the selected
ErikSom859c449 fork: split64/24 particle rows, GPU buckets/indirect fused stages,
explicit workgroup zeroing and guarded signed reductions. It is consumed only
by `app/matter_fork`, comparing frozen actual-WGSL words. Production GpuSolver shares `fork_core.slang` arithmetic and uses count/allocate/insert
plus two indirect fused stages at K2. Its production-path oracle leaf observes
eight post-integration stages and exact semantic restart. Circle presentation
remains a separate implementation gate. Current evidence/limitations live under
`out/diagnostics/pbmpm-recovery-20261022/native-batch1/`.

Mesh vertex triples and instance padding use scalar arrays so SPIR-V std430
retains the CPU offsets and 64-byte vertex / 96-byte instance strides. A float3
or uint3 in these locations adds alignment padding on Vulkan.


The actual grid material palette and grid_particle_prepare MaterialColor carrier
use 96-byte rows after the version1 optical append. Existing fields retain their
offsets; model is at 80 and RGB sigma at 84/88/92. Active program reflection and
upload descriptors check this ABI. An isolated `qualifyMaterialOptics` compute
entry reads the real TerrainVisual and reports model/sigma/S/j for readback tests;
production draw entries do not invoke it or bind its unused output resources.
Existing linear emissive RGB*strength is source function S, with absorbing
j=sigma*S, opaque source-before-stop and zero-source vacuum. No scattering,
transport refraction, terrain/matter reconstruction or scene GI is enabled here.


`grid_contour_math.slang` extracts the existing raster contour primitives, case
buffer protocol, bounded closest-point scores and saddle/label topology without
changing their function arithmetic. Grid rendering retains its prior shader
## Opaque spatial sampling entries

`grid_tiles.slang::fragmentGridSpatial` is a separate opt-in entry beside the
legacy `fragmentGridMain`. It statically uses `SV_SampleIndex` and evaluates
`fine_position`/`local_cell` at the indexed sample, preserving perspective
interpolation. Binary `occupiedPoint` reuses the existing positive-inside contour
scores, roundness and saddle connectivity; it does not sum AA ramps. Material
selection, texture patches, matched normal gradients and emission are evaluated
at each sample. The admitted Vulkan standard table supplies screen offsets.
The existing smooth 1x/legacy temporal entry remains distinct, with unchanged
304-byte frame and material-buffer ABIs. No transport shader is changed.

`grid_background.slang::fragmentBackgroundSpatial` evaluates the same layer/sky
function at sample UVs directly into the MSAA scene. The old background and
cached-copy entries remain for legacy composition. Both new entries require
separate standard-location and sample-rate capability admission; the location
flag alone is insufficient. The retired spatial runtime consumed the unresolved
scene through `spatial_msaa4.slang`, retaining signed linear HDR until grading.
The temporal Hable history and ordinary-world spatial integration were completed
in Tasks 11–14; this entry itself adds no kernel math or history. The
per-sample/source-route sample-program qualification is complete (Task 4); no
runtime selector survives Task 2.

The endpoint coefficient now has an explicit binary32 contract: RTE of the exact
mean of the two authored FP32 values in [0,1], promoted to double by the CPU
Bezier reference. Authored values are not snapped or replaced. The shared
`contourMeanRoundness` helper constructs it with uint significands, bounded
shift-jam, guard/round/sticky bits and ties-to-even packing. It requires no
floating addition, conversion rounding or new device policy. Both signed zeros
canonicalize to +0. Subnormal encoding is defined; normal-domain transport still
rejects unpreservable arithmetic. Invalid negative/nonfinite/>1 inputs produce
an invalid coefficient rather than a clamp; normal production upload/capture
validation excludes them. Raster outer/shared means and terrain curve/shared
source-partition means consume this same helper. Multi-corner visual noise
averaging retains its separate existing behavior.

`qualifyRasterContourRoundness` and `qualifyTerrainContourRoundness` call the
actual raster outer/shared functions and terrain curve/shared-source functions.
They use a 16-byte count/reserved constant, uint2 pair IDs (8-byte stride) and
uint4 output (16-byte stride). Grid input remains the actual 96-byte palette;
terrain input remains 8-byte vertices. These isolated entries do not change
production draw/trace bindings. The app coefficient leaf initializes only its
shader/RHI boundary, tests literal independent expected bits on both paths and
owns separate ordinary/Core+GPUAV registrations. Tests and old-entry interface
compatibility still require execution; no new geometry or receiver admission is
claimed from coefficient construction.


`spd_bloom.slang` supplies three compute entries to `PostProcessPass`:
`prefilterBloom` (exposure-aware ceil(R/2) soft-knee + Karis bright pass),
`downsampleBloom` (one unweighted 2x2 box dispatch per coarser level, with taps
clamped to the last valid texel), and `upsampleBloom` (the weighted descent:
`w_l*raw_l + tent(result_(l+1))`, with the coarsest level applying `w_last`
alone; the tent is separable [1,2,1]/4 sampled one coarse texel apart with a
linear clamp sampler). Level 0 is ceil(R/2); later levels are ceil(previous/2),
stopping at 1x1 or six separately allocated RGBA16F images. The reflected 64-byte
`BloomParameters` block carries source/src/dst extents, exposed-unit threshold and
soft knee, manual EV, level, the upsample-add flag and reserved words. The prefilter samples the 1x1
adapted offset and converts threshold/knee to scene units exactly once:
`threshold/E`, `0.5/E` with `E=exp2(manual_ev+adapted)`.
`post_process.slang` adds level 0 once, scaled by `m_bloom_intensity`, before the
single exposure/AgX evaluation.

## Retired reconstruction programs

`temporal_resolve.slang` and `temporal_2x.slang` were the legacy screen-history
and reactive-MRT reconstruction programs. Task 2 removed them with `RetroPass`;
`engine.app:renderer.reconstruction_pass` now owns the always-on Hable temporal
reconstruction, its history pair and its sample-rate scene entries.
`spatial_msaa4.slang` remains as the independent kernel/sample-pattern
qualification asset, not a runtime selector.
