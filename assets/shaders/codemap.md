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
tinted or per-cell quads, one instance per visible chunk-sized tile). `retro_present.slang` and
`temporal_resolve.slang` supply independent picture treatment and optional temporal reconstruction
through `RetroPass`. The former `triangle.slang`
reference asset was deleted. Each pass owns its shader program;
`RadianceCascadePass` also loads the bounded transport qualification program,
and diagnostic fixtures load their qualification assets directly.

`grid_visibility.slang` supplies GridPass compute reset and cull entries. It
matches the CPU reference's conservative boxes and frustum planes, atomically
appends at most 1024 visible IDs, and writes one indirect draw record. The tile
vertex entry optionally resolves instances through those IDs. Reset, cull,
barriers and drawing share one encoder and submission.

`radiance_cascades.slang` exposes `buildVacuumHierarchy`, `traceOptical`,
`cascadeTop` and `cascadeMerge`, with explicit packed-storage wrappers
`traceOpticalPacked`, `cascadeTopPacked`, `cascadeMergePacked`, for bounded FP32 cell-constant optical/RC qualification owned by `RadianceCascadePass`.
Scalar cell/ray/result/constant-buffer strides are 40/24/32/32 bytes, checked by
CPU layout assertions and active-target shader reflection. GPU origins are local
fine cells, coefficients/source density are per metre, and opaque source events
precede termination. `cascadeTop` clips boundary-complete padded-probe rays;
`cascadeMerge` traces from the common parent-bin near origin to each angular
child's four upper starts, composes each pair, then applies bilinear and angular
weights. Direction/cascade buffers have 8/64-byte scalar protocols, and true
floor/ceil support skips zero-weight dependencies. A flattened uint vacuum tree
with 16-byte level metadata is built from each immutable optical capture, with
inter-dispatch barriers and disjoint level writes. Only fully in-domain explicit
vacuum blocks skip to their exact forward exits; odd/degenerate padding cannot
skip. Build RW and trace SRV views are bound to separate entry roots. The FP32
baseline and hierarchy passed system Khronos core/GPU-assisted Vulkan validation.
The new packed wrappers share FP32 trace/merge evaluation, store six independent
binary16 channels in three uint words plus per-slot status in the fourth, and
retain local span counts only in a separately admitted bounded diagnostic plane.
Explicit bounded integer RTE packing and bit-exact half expansion replace native
half intrinsics whose observed rounding did not satisfy the selected contract.
The codec preserves normal/subnormal ties and carry, and retains nonfinite/sign
information on decode for validation. Its cost is unmeasured.
There is no automatic fallback or shared-exponent/scalar-transmission encoding;
finite/range failures propagate as status. Reflection checks the actual 16-byte
record ABI; raw words remain qualification-only bit observations. The native
probe failure is preserved; deterministic RTE passed Debug real-layer and focused
Release gates. This independently authored tree is not a verified v22
bitmask/jump-table port, measured speedup or production publication.

## Design

`grid_particle_prepare.slang` is GridPass-owned resident-particle presentation:
numerical64/readonly24/metadata32/packed32/frame128 byte protocols. Packing uses
the committed hot/cold row identity and physical metric; three stable visibility
scan/compaction dispatches produce ordered IDs and indirect arguments without
reordering solver storage. Conservative culling encloses both the liquid kernel
and angular solid support, using `grid_liquid_kernel.slang` and
`grid_solid_kernel.slang`. `grid_tiles.slang` shares the particle quad helper.

Solid entries `vertexSolidSplat`, `fragmentSolidMaterial`,
`fragmentSolidCoverage` and `fragmentSolidComposite` reconstruct world-XY
octagonal grains. Equal-area apothem and a separate calibrated solid support
retain angular silhouettes without changing physical inventory. Occupancy sums
all solid elements; nearest normalized grain distance selects one material ID
per field texel. Native composition bilinearly filters categorical coverage,
shading each distinct neighboring material once and mixing premultiplied linear
color. Empty labels are excluded and weights renormalized; the independent
occupancy mask retains the silhouette. Uniform interiors shade once. Composition invokes the exact terrain
`materialSurface` path with explicit world gradients, including authored normals
and optical properties. There is no liquid smoothing/height-normal treatment in
this lane. Solid and liquid fields remain separate until composition; both use
coherent displaced-contour derivatives before varying rejection and defer
terrain/material work until occupied pixels survive. Gases keep their sampled
disk alpha and cold/palette RGB. Liquid entries `vertexFluidSplat`,
`fragmentFluidMaterial`, `fragmentFluidThickness`, `vertexFluidComposite` and
`fragmentFluidComposite` reconstruct a connected 2D optical surface. Closest
normalized kernel distance selects one element; accumulation and filtering never
mix distinct elements. Raw/filtered fields use R32Float to remain finite for
up to 1M overlapping particles. `grid_fluid_filter.slang` owns `filterLiquid`, an
8x8-thread shared-memory, element-guided 5x5 compute filter with a 16-byte frame.
The composite reconstructs the bounded half-resolution field bilinearly, preserving
immiscible boundaries, and evaluates world-plane intersections per fragment.
A bounded optical height derived from density supplies rounded normals without
another texture. Shared support, amplitude and isolevel preserve kernel mass and
the isolated radius while filling deformed sampling gaps. Empty field pixels
skip convolution after the group barrier; empty composite pixels skip field and
terrain queries after coherent displaced-contour derivative preparation. Fresnel and reflection use the per-fragment camera ray;
roughness controls a stylized sky/key response and nearby backdrop reflections.
Authored depth drives stronger refraction, limited to twelve screen pixels;
reflection has its own twenty-four-pixel bound. Terrain occlusion, material
opacity/IOR/roughness/emissive and the optical backdrop remain shared contracts. Resource extents, costs and fence ownership
are recorded in the renderer codemap. No solver state or CPU particle readback
is required by presentation.

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
- `grid_tiles.slang` per-cell lane: when payload flag bit 0 is set, the fragment resolves the cell
  through packed `StructuredBuffer<uint> g_cell_materials` (8192 words per chunk, halfword per cell,
  `0xFFFF` discards); otherwise the payload material is used. Catalog terrain
  resolves bindless textures and metre-period UVs; diagnostic modes use an owned
  palette. The low-three-bit tint remains only for explicit legacy fixtures.
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
- `retro_present.slang` samples the chosen scene resolution with clamped pixel-center nearest taps
  (`SampleLevel 0`). Optional palette mapping converts linear→sRGB, applies a 4×4 Bayer threshold,
  selects the DawnBringer-32 entry by luma-weighted squared distance, then returns linear color.
  Palette bypass preserves full scene color independently of pixelization. Its
  `ConstantBuffer<RetroParameters>` is 528 B (32 float4 palette entries, float2 source extent,
  float dither strength, uint palette-enabled flag). `uniform SamplerState.Handle g_sampler`
  remains the present entry's only ordinary-data parameter. Entries: `vertexRetroPresent`,
  `fragmentRetroPresent`.
- `temporal_resolve.slang` uses a 160-byte constant-buffer mirror: current inverse and previous
  unjittered view-projection, physical current/previous jitter offsets, extent, flags and reserved
  word. Four-phase 4x MSAA color accumulates in linear RGBA16Float history before palette mapping.
  Catmull-Rom RGB and depth validation share the full 4x4 point-load footprint. History alpha is
  orthographic device Z or perspective reciprocal clip W; negative alpha marks background.
  Camera motion requires a coherent affine metadata plane over all contributing taps. Auxiliary
  reprojection aligns depth jitter phases separately from RGB motion; cuts and full world/policy identity or
  projection-mode changes invalidate history. Shader entries are `vertexTemporal` and
  `fragmentTemporal`, with no sampler or ordinary entry-point data. Composite parallax backgrounds
  use unjittered cubic spatial filtering and reject history during motion. Near/above-Nyquist
  patterns and dynamic silhouettes without object-motion vectors retain reconstruction limits.
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
contour/diagnostic buffers, and the bindless texture heap/shared sampler. `RetroPass::initialize`
loads `<content_dir>/shaders/retro_present.slang` (`vertexRetroPresent` + `fragmentRetroPresent`),
then binds the 528-byte palette/extent constant, the selected scene view, and one sampler before each
fullscreen present. Supported devices also load `temporal_resolve.slang`; its pass-owned targets,
roots and history are reused only through accepted-submit fence accounting.

## Integration

- **Consumers**: `engine.shader` (compile), `engine.rhi` + `TrianglePass`/`GridPass`/`RetroPass` (pipelines +
  constants), `game` demo.
- **Depends on**: Slang compiler; `engine.math` matrix-layout/handedness conventions (row-major,
  row-vector, left-handed +Z forward, [0,1] depth).
- **Provides**: the runtime bindless mesh shader asset for the scene pass, the grid-tile shader
  asset for the debug-grid pass, and the retro present shader asset for the low-resolution palette pass.
- **Known limitations (documented in-shader, deliberately unfixed)**: the model matrix must be a
  rigid transform or a uniform scale, since directions are rotated with `mul()` rather than the
  inverse-transpose; and `FrameConstants` carries four fields (`m_view`, `m_projection`,
  `m_inverse_view_projection`, `m_viewport_size`) that this shader never reads but that the CPU
  `sizeof == 288` assertion and `uploadFrameConstants_` still require.

## Key Files

- `mesh_bindless.slang` - the live bindless static-mesh vertex/fragment program.
- `grid_tiles.slang` - the live debug-grid tile vertex/fragment program (`vertexGridMain` + `fragmentGridMain`).
- `retro_present.slang` - the live picture present program (`vertexRetroPresent` + `fragmentRetroPresent`).
- `display_agx.slang` - local opt-in HDR display functions included by RetroPass:
  attributed Filament scalar Punchy approximation, explicit linear-sRGB/Rec.2020
  conversions and final SDR gamut clipping. See `third_party/FilamentAgX/README.md`.
- `temporal_resolve.slang` - optional MSAA temporal reconstruction (`vertexTemporal` + `fragmentTemporal`).
- `spatial_msaa4.slang` - standalone signed linear-HDR MSAA4-to-2x spatial
  reconstruction prerequisite, currently loaded by asset fixtures. It uses an
  admitted sample-index mapping, continuous diamond weights and corrected
  lower-bucket sums; source/derivation are in `third_party/HableMSAA/README.md`.
  The raster threshold probe establishes finite coordinate brackets for tested
  pixels, not an exact/global native pattern guarantee. Runtime pattern
  admission, real terrain sample coverage and RetroPass integration remain open.

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
compile policy. `radiance_terrain.slang` reuses those hit-source functions and
implements equivalent implicit corner conics for noise-free opaque geometry.
Its new traceTerrain/cascadeTerrainTop/cascadeTerrainMerge entries in
radiance_cascades are compiled only as retained PRECISE-linked Vulkan programs.
Local outward FP32 bounds, normal-domain/FTZ guards, squared sqrt bracketing and
explicit numeric_ambiguity status protect scalar dual-cell traversal. The
existing cell and packed wrappers specialize the shared RC evaluation with
terrain=false; they do not bind terrain resources. Terrain metadata is 8 bytes per
sample, constants 32 bytes, shared points/cases 8/72-byte strides. Actual shader
reflection and buffer elementSize checks cover these protocols. This authored
qualification path has passed physical-ray and ambiguity leaves on the prior
frozen source; the old terrain RC leaf still reported numeric_ambiguity at row 28.
Exact linked SPIR-V dumping remains explicit and test-only.

The new terrain-only RC refinement shares one parametric traversal with the
physical-metre trace wrapper. Terrain top rays use raw uploaded direction and
fine-cell lambda limits. Local TerrainPoint/Line values retain probe+near*d_raw
endpoint expressions and trace connectors as S+t*(E-S), without a rounded target
or normalization. Local half-grid subtraction and common-term cancellation
preserve provable identities; only singleton-zero directional bounds prove an
identical connector. Bit/range guards precede identities, and all unresolved
sign/order/positive-span cases remain numeric_ambiguity. The host checks raw
length>=.5 via a component bound and uploads twice the estimator's complete
padded-probe distance bound as terminal lambda. These local values introduce no
new GPU buffer or constant ABI, and generic-cell/half paths are unchanged. The
new schedule awaits numeric/IR/execution gates; it is not yet receiver contact
handling, production world transport, physical angular admission or a speedup.

Hit-source selection carries the full first-event enclosure. Common-source
multi-label hits and singleton evaluation preserve the existing point-score rule;
non-singleton two-label straight cases require conservative certificates of its
warp/containment signs and positive score separation. Other differing-source
ownership is explicit numeric_ambiguity. General curved differing-source support
remains required before ordinary-world/emitter integration. Anchor subtraction
uses direct/reverse Sterbenz proofs with raw normal/FTZ guards and conservative
fallback, without snapping. Both candidate and exact reverse must remain valid
before equality certifies an exact difference. Same-component PRECISE /
NoContraction and no RelaxedPrecision remain mandatory inspection gates.

Terrain failure status retains the existing low byte, including bit32 for
numeric ambiguity. Seven diagnostic category bits are local at bits8..14 and
inherited at bits16..22: clip, initial dual-cell ownership, DDA/progress,
root/coverage, event ordering, source ownership, arithmetic/input, respectively.
Decode each lane as `(status >> 8) & 0x7f` / `(status >> 16) & 0x7f`.
Direct/top traces set local categories only at existing failures. Terrain RC
merges preserve this invocation's connector categories locally and fold both
parent lanes into inherited; multiple failures union categories without claiming
a unique fault or pairing a category with a particular dependency. Speculative
coverage failures discard their tags together with the old scratch status when
the existing root fallback proceeds. Generic-cell/packed status and every
successful arithmetic/query path remain unchanged; no buffer or ABI is added.

ORDER detail flags at bits24..27 name the existing requested-span,
root-versus-span, sorted-cut separation and representable-interior-middle
rejections, respectively. These flags union across connectors and all parents;
they do not identify a unique dependency. At C0 with inherited ORDER,
bits28..30 select the first positive-weight upper dependency WITH ORDER in
the unchanged loop order, using ordinal `child*4 + y*2 + x`. Bit31 reports
whether that selected C1 also has inherited ORDER. For the unchanged 2x2
cascade failure at C0 record28, ordinals0..7 map to C1 records
`{8,24,40,56,9,25,41,57}`. C0 selection metadata is not inherited from another
level; the site-bit union remains independent of this selected dependency.
All detail bits accompany existing failures, preserving successful status0.

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
flag alone is insufficient. RetroPass consumes the unresolved scene through
`spatial_msaa4.slang`, retaining signed linear HDR until subsequent grading.
No kernel math, temporal history or ordinary-world enablement is added here.
Actual per-sample/source-route qualification remains pending.

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


`terrain_receiver_types.slang` shares the 32-byte integer feature witness plus
visual-only FP32 centre,16-byte receiver result, 8-byte cell range and 16-byte
component/candidate range protocols between the terrain producer and actual Grid
shader. `terrainReceivers` reuses symbolic `TerrainPoint` midpoints and owned
curve witnesses, consumes the unchanged C1 schedule, and composes before weighting.
Only its proved zero contact is factored; complete positive occupancy and later
hits remain certified. Existing ordinary trace/RC/packed wrappers keep their
contracts. The receiver entry is linked PRECISE and created lazily only when used.

The enabled Grid receiver path shades actual authored linear albedo with the
completed angular mean plus separate linear source S. It supplies no fixed ambient
or legacy local directional/specular response; normals and rough specular remain
later consumers of retained directional C1. The geometry/visual identity is checked
against actual resident publication, and draw roots strongly retain their immutable
GPU generation. Per-cell conservative component lists drive visual-only FP32
nearest-midpoint continuation with stable feature-key ties, never a global fragment
receiver scan or transport approximation. This source is authored; execution and
first closed/open game captures remain pending.

## Opt-in cut-face interpolation experiment

`grid_tiles.slang::gridReceiverMean` keeps receiver enabled1 as the original
nearest lookup; enabled2 selects the fixed normalized half-cell support kernel
after the same component/key search. Enabled0 remains off in material shading.
The existing32-byte constant/buffer protocols are unchanged. Source emission,
material coverage, opacity, receiver transport positions and RC results are not
interpolated or recomputed. Coincident centre conflicts preserve lowest-key
selection. Its unchanged nearest receiver anchors the side; a blended candidate
must have strictly positive outward-direction dot against that anchor, excluding
opposite and perpendicular faces. A medial-line discontinuity is intentional when
incident sides differ. The half-cell radius, weights and candidate support remain
unchanged, and the policy is visual-only rather than a new transport ray.

Feature offsets24/28 now carry normalized visual-outward XY binary32 bits in the
former reserved u32 slots; the32-byte stride and all bindings remain unchanged.
Capture uses the oriented Bezier midpoint tangent B-A, reversing the complementary
case for connected saddles. Its checked axis/diagonal normals have equal-magnitude
nonzero components, so the shader tests true dot sign with a small integer
component-sign dot; both signed zeros are zero. This avoids floating contraction
residuals for perpendicular faces without changing the normalized direction data.
The helper is restricted to this captured midpoint domain, not general normals.

`qualifyGridReceiverContinuation` is a compute-only observation entry calling the
actual lookup and sample-addressing path, returning raw linear incident mean
before albedo/S/post. It uses16-byte scalar input/count records and float4 output;
ordinary draw entries never bind or invoke it. App tests separate kernel/site/seam
equivalence from the unchanged1% blocked-side quality gate. Unrestricted blending
previously failed16.15% leakage (nearest0); facing-restricted source awaits the
same masks and independent oracle. No physical admission or4K60 claim is implied.

## Mapped diffuse angular receiver response

Optional `terrainReceivers` angular output stores16 spatially composed RGB/status
records per receiver in a separate16-byte-stride UAV. The existing64 connector
queries and original mean accumulation order are unchanged. The32-byte producer
constant replaces reserved0 with angularOutput; mean-only callers bind the existing
mean buffer as the unused valid angular UAV and never enter the angular branch.
Completion and generation ownership validate/retain all bins before Grid use.

Grid's receiver constant is now48 bytes; the added row contains response,
directionOffset, directionCount and reserved. Separate16-byte angular and8-byte
direction SRVs are reflected with scalar offsets and bound to owned generation
resources, or established unused dummy bindings for flat/off response. The same
matched normal-map sampling runs only for legacy or explicit mapped response.
The exact front normal keeps the old mean result; separate S and coverage do not
change. A single facing-restricted candidate pass reconstructs all16 RGB bins.

`terrainReceiverDiffuseLift` in the shared receiver protocol is the actual
normalized uniform-elevation coefficient, averaged as sum(K*L)/16, without another
1/pi. It uses the closed positive-A expression, explicit B=0 and negative-A atan2;
small x<=.25 uses a positive-factor alternating series with its explicit omitted
term bound. The helper normalizes uploaded XY bin directions without camera
rotation. Independent CPU quadrature drives actual helper/lookup probes, including
constant fields, weighted RGB, facing isolation and unchanged flat/front bits.
The qualification probe input is now32 bytes (position/material plus normal and
operation); ordinary draw entries never invoke it. New ordinary and Core/GPUAV
test registrations are authored but unrun. This adds no specular/media/physical
quality admission or performance claim.

## Exact-view RC backwall reconstruction (authored, execution pending)

`radiance_cascades.slang::terrainBackgroundMeans` reduces the optional completed
C0 field to16-byte RGB/status rows. `terrainBackgroundView` uses a96-byte block
(row-major inverseVP at0, raster/source extents at64, world origin/metre metric
at80),16-byte incident rows, and eight scalar counter words. Output ordering is
pixel-major: cached centre then standard indexedMSAA4 positions(3/8,1/8),
(7/8,3/8),(1/8,5/8),(5/8,7/8). Host reflection checks the actual mirrors.
`grid_background_world.slang` shares the existing receiving-plane unprojection.

`terrain_query_shared.slang::traceTerrainCellConnector` reuses the original
Bezier/clip/source certificates for a proven in-domain closed dual-cell square,
with a vacuum-only identity fast path. Existing eligibleFP64 refinement remains
local and explicit. Candidate errors are counted and prevent publication;
provisional error output is never a darkening fallback. The finite backwall
plane is the exact C0 convex hull; outside samples count as declared far void.
`grid_background.slang` consumes only the already validated view using its16-byte
lighting block and incident SRV. Legacy unlit bindings use the existing unused
uniform-branch dummy convention; old304-byte GridFrame is unchanged.

## Native-cell world-field experiment

`world_radiance.slang` belongs to `WorldRadiancePass`, not Renderer. It has no
Bezier, precision-certificate or FP64 dependency. `solveWorldCascade` executes
coarsest-to-C1 native half-open DDA with vacuum-only hierarchy skipping and RGB L/T
composition; `refineWorldRadiance` composes64 local footprint connectors before
spatial weighting and projects their actual directions into signed order4 CH with
DC geometry correction/Lanczos window. One known phase is shared by every level.
Only actual terminal world escape appends explicitly declared sky; a finite miss
is vacuum identity and a too-short terminal query fails. Integer crossed faces,
negative ownership and a width+height-derived progress bound replace silent step
caps. All group lanes reach counter barriers, including out-of-domain lanes.

`traceWorldQueries` and `qualifyWorldDiffuse` are isolated qualification entries,
with reflected32-byte interval and32-byte raw/display normal output rows. The latter
calls the same K16 diffuse consumer and existing `terrainReceiverDiffuseLift` helper;
it reports finite truncation undershoot and clamps only final physical E/pi. The
producer's CH record is112 bytes (27 scalar floats plus status); source palette is
40 bytes (RGB S/kind/RGB sigma/RGB j) and packed ID words are4 bytes. The matching app fixture and explicit
ordinary/Core-GPUAV experiment registrations are in `App.WorldRadiance.Tests.cpp`
and tests/app/CMakeLists.txt. These sources are authored, not yet GPU-qualified or
wired to real Grid foreground/backwall shading.

The candidate refine entry now reads explicit spacing1/2 from word12 of the existing
16-byte `WorldDispatch` (formerly reserved); reflection verifies that offset and
field name. World parameters remain48 bytes, intervals32 bytes and signed CH112
bytes. Spacing2 remains default. Spacing1 is a labelled centre-aligned control,
not a claim to be literal v22; the C1 footprint stays +/-4 native cells in both
modes. CPU support closure, output allocation and immutable generation metadata
use the same selected spacing, with actual capacities admitted before allocation.


The native1 full cascade subsequently failed56.87% open L1; its C1-terminal
control failed69.42%, both with zero closed leakage. Those measured controls
remain rejected; no footprint/phase tuning or Grid wiring follows from them.
The next explicit `WorldRadianceTransport::straight256_reference` mode is a
small-scene native-cell-centre reference, not a replacement scalable RC solver.
Default `cascade_diagnostic` remains unchanged. Straight mode requires spacing1
and phase.5, stores its transport choice in the immutable generation, and writes
exactly width*height signed112-byte CH rows at i+.5 (minimum index0). It uses the
existing refinement pipeline's uniform mode branch, existing hierarchy/DDA and
first-opaque/actual-world-escape boundary. It uploads256 fixed midpoint directions
and one terminal descriptor, skips every coarse dispatch/root/atlas, and accounts
one32-byte unused parent binding required by reflection. At64x36, exactly589824
straight queries are admitted; publication requires that exact completed count.
The CH projection uses actual uploaded directions and the same DC-centering/window,
with exact common-input DC and zero AC. No camera/view resource is created.

The existing single shared GPU fixture retains constant/DC/normal/admission/lease
checks, then evaluates all12 fixed points in each unchanged door state. It reports
1024-direction physical references, independent slab references using the same256
uploaded binary32 directions/source values, and GPU DC separately. Sampling error,
GPU traversal agreement (abs1e-5+rel1e-4), open physical15% L1 and closed1% leakage
are distinct outputs and assertions. Both native spacing failures remain archived;
passing this bounded straight reference would not establish scalable RC, fine
contour quality, full-world storage or GPU timing. Foreground incident-side
reconstruction and Grid/Editor wiring remain unimplemented for this mode.


The small-scene showcase shares signed CH/K16 through `world_radiance_types.slang`
(producer and consumers) and `grid_world_radiance.slang` (48-byte draw constant,
112-byte rows). Direct producer constants are64 bytes, with selected maximum S at
48/52/56. Pre-overflow magnitude limits plus8H+2S reject unsupported convolution
ranges through the existing aggregate status. Old contour/FP64 shaders and their
precision contract are unchanged. Both headers use existing directory staging.

The rejected transverse plane-start qualification arm is retired from active
execution, alongside fixed16 and terminal-C1. Their exact frozen source and raw
REJECTED evidence remain under the corresponding diagnostics artifacts. The
mandatory straight256/independent1024,15%/1%, normal/material/status/lifetime
controls remain active in the same shared native fixture.


The native opaque material experiment uses one selected exposed-side CH probe
for both K16 diffuse and a four-order Poisson reflection filter. For unit N and
orthographic V=(0,0,1), complex powers of
`(1-roughness^2)*(R.x+i*R.y)` filter the signed pairs, then only the completed
reflection is clamped and multiplied by saturate(R.z). A pole returns DC exactly;
no azimuth/trig/quadrature is introduced for specular. IOR-derived dielectric F0
blends toward linear albedo by metallic and uses Schlick at N.z. Metals skip K16
but still validate normal/material/CH inputs. Both world lobes use the existing
flat/mapped response choice. Deep opacity has zero world reflection; the mapped
render-only0.04 key and own S remain once, separate from transport/background.
`kNativeMaterialResponse=true` is the single temporary Grid staged-literal A/B
control. False retains the prior diffuse native expression; no config/ABI changes.
P<=H and D<=3H keep world reflection<=3H, so the existing8H+2S<=65504 admission
still covers3H+S+0.04. No AC clamp or source/transport change is introduced.
The existing `qualifyWorldDiffuse` pipeline keeps level0 pure diffuse; level1
uses fixed material presets in the high16 query coefficient bits and the row in
low16. Its16B inputs/32B outputs,112B CH and16B dispatch are unchanged. The same
shared native test leaf adds analytic pole/signed-harmonic/roughness/horizon,
dielectric/metal, key/S, high-bound and invalid-input status controls without a
second application/device or CPU BRDF pipeline. Actual pipe/stone pixel and
MSAA4 material-draw cost comparisons remain required and unmeasured.

`grid_tiles.slang::gridWorldRim` implements the declared local16-neighbor incident
policy: matching native material cells, actual in-crop vacuum neighbors, nearest
native face and stable key; deep opacity is0 incident+separateS. The actual
`qualifyGridWorldRadiance` entry reuses that draw consumer and the backwall crop
consumer inside the existing shared RHI fixture. The background shader samples
current-camera worldXY via the checked orthographic plane helper and retains
full CH, with explicit transparent far void beyond[0,width]x[0,height]. Edge-centre
clamping is restricted to the interior half-cell strip. No new rays, light list,
exact-view allocation or legacy unlit policy is introduced. Actual fine-image,
GPU cost and interactive validation remain pending.


The world reference producer's existing64B `g_world` protocol uses scalar60 as
`maximumResponse` (float), replacing an unused reserved u32 without changing
stride. For straight256 it compares the existing conservative8H+2S response
bound against the requested ceiling after its pre-overflow magnitude checks.
Normal convolution, signed coefficients and source values are unchanged.
The bound covers Grid's admitted RGBA8 UNORM/sRGB reflectance and tint, bounded
stochastic albedo, convex B-spline/backdrop and spatial interpolation, with surface
S added separately. Half-float presentation requests65504; the default FP32
control still accepts100000 sources and only the half-qualified publication fails.


The native small-scene terrain material branch has a bounded render-only cut-face
term `kNativeCutFaceIncident=0.04`, modulated by the owned mapped normal against
normalize(-0.4,0.5,1). Terrain reflectance multiplies it and own S remains separate
and single-counted. It never modifies CH/queries, feedback, gameplay, background
or legacy paths. Isolated staged literal0/0.04 variants provide the matched
backwall-only/backwall+fill capture control with no new API/configuration.
The existing half admission8H+2S<=65504 dominates actual3H+S+.04<=32752.04, with
ample half-format headroom; source/coefficient values and guard are unchanged.

### Temporal backwall near-ray correction

`refineViewportContact` uses the current terrain contour mask. Each of 16
angular bins traverses consecutive screen pixels up to two native cells or
16 pixel crossings. A supported opaque hit replaces the bin's directional
radiance with surface emission and diffuse reflection. The block stores the
signed difference divided by 16 and eligibility bits. The backwall interpolates
differences only along observed open connections. A ray that leaves the screen,
exhausts its crossing budget or encounters unsupported optics keeps its world
radiance. The correction has no frame history.

### Centre-density screen-contact experiment

`grid_tiles.slang` adds `vertexScreenContactMask`/`fragmentScreenContactMask`
through the same displaced `shadeGrid` material selection, and
`refineScreenContact` for the bounded16-direction/8-tap/2-native-cell backwall
pilot. `grid_screen_contact_types.slang` is the shared32-byte parameter/16-byte
RGB+status protocol and centre-result consumer. `grid_world_radiance.slang`
provides signed directional CH interpolation at the actual far endpoint/screen
exit; it does not trace or mutate the world field. `grid_background.slang`
consumes the result once per centre-density pixel in both existing background
paths, with ordinary world-only lighting outside the declared/capped pilot ROI.
Negative signed terms survive through convolution; unexpected invalid rows are
explicit magenta debug failures. The8-tap kernel is approximate, not exact
contour intersection. Default-off foreground/world solver semantics are intact.
The existing shader directory staging includes the new pure header; no new C++
module or target is introduced.

## Native direct-window terrain-emission measurement

`WorldRadianceTransport::direct_window` uses the same256 midpoint directions,
first-hit native DDA, signed CH projection and explicit sky. A32x32 receiver page
has one eight-word conservative emitter-direction mask. An omitted black-sky
bin is exactly zero and still carries its original1/256 projection weight;
nonblack sky traces all256. Source bounds include opaque S and homogeneous j.
No footprint warp, parent blending, reflected-light cascade or history is added.

Phase-A `discoverWorldFaces` is an additional entry in `world_radiance.slang`,
owned by the same WorldRadiancePass program family. `worldParentSample`,
`worldCascadeGeometry` and `worldRefinementGeometry` are shared by the existing
solve/refine entries and discovery, including positive-weight parent guards.
Discovery enumerates terminal rays and connectors without reading radiance
parents or using authored source values to prune dependencies.

The indirect endpoint candidate stores every interval at
`worldProbe+direction*nearCells`. `worldBounceCascadeGeometry` supplies each
positive corner's FOUR distinct child-origin connectors to both discovery and
reflected solve; each child is composed before weight/4. Reflected refinement
and discovery connect the receiver to the exact selected C1 origin, with no
footprint exit. C1/C2 cost16 traces per record, C3 one and refinement64 per row.
The primary diagnostic footprint and native direct256 paths remain unchanged.

`worldTraceHit` is the single compact native DDA; `worldTraceRaw`/connector
wrappers preserve direct hit-cell L/T while exposing optional incoming-face keys.
At an opaque two-plane hit, a sole exposed face wins; two exposed faces choose
larger absolute direction then X. With neither exposed and a transmissive
predecessor, the first opaque axial neighbour on that dominant axis owns BOTH
the face and future material response. Vacuum-block exits recover simultaneous
transverse native-plane events before constructing the incoming predecessor;
block-boundary axes alone are insufficient. Direct hit cells and L/T stay intact.
Exterior is transmissive. Strict initial
opaque has no incoming face; diagonal-vacuum grazing remains no hit. Keys are
`(cellIndex<<2)|side`, with side order -X,+X,-Y,+Y. Qualification's existing query
reserved value1 returns the key in the uint segments lane; normal rows retain
their original segment count. No new qualification program is created.

New scalar `WorldDiscoveryDispatch` is32B (spacing/capacity at16/20B, bitset words
at24B). Discovery-only UAVs are bitset, key list and four-word state: unique count,
overflow, per-quantum face-hit occurrences, and generation-distinct faces outside
the requested world-registered query rectangle. Host aggregates occurrences in
u64; each quantum resets only that occurrence word. First-set winners count each
outside face once, conservatively offscreen (not exact visible-pixel coverage).
Atomic capacity failure sets status/overflow and cannot become a complete result.
Existing direct buffers, CH rows and transport are not replaced. The explicit
Editor/Game measurement hook now awaits this separate immutable result; moments
and indirect publication remain held, and actual4096 acceptance is still unrun.

The40-byte palette retains homogeneous per-metre sigma/j and opaque S. Ordered
segments use the canonical stable Beer-Lambert source integral over actual metre
length. Only true vacuum participates in hierarchy skipping. Finite/ranged L/T
and overflow status remain visible; permitted FP32 attenuation FTZ is covered by
a once-per-source conservative omitted-radiance bound against1e-5, not a general
precision certificate. Inputs outside that numerical domain reject explicitly.

The32-byte dispatch selects one page into a private dense output rectangle.
Counters stay u32 per bounded dispatch/workgroup; completed host aggregates are
u64. No row is exposed before every requested page passes fence/status checks.
The active qualification entry is level0 raw L/T, reused for independent scalar
zero/weak/thick/coloured/zero-sigma emission and behind-opaque controls.

Native spatial terrain/final liquid/solid/disk programs are distinct from legacy
programs even when both signatures are4x. They evaluate actual sample positions
and displaced terrain coverage, preserve single-sample envelope reconstruction,
and use premultiplied ONE/INV_SRC_ALPHA final blending. Emission is additive once
without S/a. Zero displacement preserves actual destination-sample identity;
displaced reads use the declared filtered RGBA16F backdrop. Absorbing/moving
surfaces sample incident CH at their actual point; only opaque terrain uses rim
probing and the independent.04 key. Aggregate transparent particle emission HDR
admission remains UNQUALIFIED; finite measured-scene/sample checks are required.

### Opt-in single-bounce world window

`world_radiance.slang` specializes the SAME native DDA and C1–C3/refinement
geometry for reflected-only intervals. A generation-local ordinal+1 CAS table
compares full discovery keys; invalid/duplicate/full/missing entries set failure
status. Five lazy pass-owned entries build the table, compute RGB32F outward
primary incident moments, solve/refine reflected-only radiance, and combine it
with pristine direct signed CH before the unchanged finite/HDR admission. The
52-byte scalar material row adds bounded diffuse albedo; scalar RGB moments are
12 bytes and bounce dispatch constants16 bytes, checked by host reflection.
Primary wrappers preserve S/sky/j. Secondary intervals retain Beer–Lambert T but
contain no authored S/sky/j; only the keyed owner's A*moment supplies a hit.
Their boundary is absorbing: strict exterior starts terminate black, exact
boundary starts use native forward ownership, and the first physical world exit
sets T0 while retaining accumulated L. Discovery uses the identical termination
without accessing moments. A path cannot leave the world and reenter behind a
wall; missing required in-world keys remain errors.
Cardinal moments use the existing normalized K/256 over128 outward bins; there
is no second/pi, key light, recursive feedback or per-fragment lookup.

The paired first-hit qualification keeps `traceWorldQueries` level0 primary
semantics and adds level1 reflected-only tracing with the SAME native DDA and
complete-key lookup. The shared test fixture unions all256 receiver-ray keys,
then invokes the existing lookup/moment entries on its own bounded control
buffers before256 secondary evaluations. This is a diagnostic moment control,
not a production transport mode or acceptance of the rejected RC leakage gate.

## Retro bloom (SPD)

`spd_bloom.slang` supplies three compute entries to RetroPass: `prefilterBloom`
(a half-resolution soft-knee + Karis bright pass), `downsampleBloom` (one
dispatch reducing mips 1..5 with a workgroup-local 2x2 box over a 32x32 mip0
tile, per-level bloom weights folded into each mip write), and `upsampleBloom`
(a per-level 3x3 tent accumulating each mip back into mip 0). All three share a
reflected 32-byte `BloomParameters` block: uint2 source extent at0, uint2 mip0
extent at8, exposed-unit threshold at16, soft knee at20, mip count at24, and the
upsample source level at28. The prefilter also samples a 1x1 `g_exposure` offset
and divides the threshold/soft knee back to scene units (`exp2(-adapted)`), so
the bright pass is exposure-aware. A single RGBA16F texture holds the mip chain,
bound as per-mip UAV views; `retro_present.slang` adds mip 0 once, scaled by
`m_bloom_intensity` (default 0.125), before the single exposure/AgX evaluation.

## Temporal2x and final reactive MRT programs

`temporal_2x.slang::resolveTemporal2x` uses a reflected192-byte scalar/matrix
block: inverseVP0,previousVP64,current/previous raster-jitter UV offsets128,
raster extent144,output extent152,index table160,flags176 and scalar reserved
words180/184/188. It preserves Hable's four distinct current outputs and reads
the seven actual raw color/depth/reactive taps of each bucket. History remains
indexed by unjittered output pixels. All nonzero Catmull-Rom taps, including
negative lobes, reject reactive metadata before plane/depth interpretation.
Current reactive support writes-2; background is-1; old reactive rejection
writes current metadata so vacated pixels recover. Scene-linear final history
feeds bloom before AgX/exposure and native UI.

Grid terrain/background/fluid/solid/disk and mesh reactive entries emit the
existing final color plus R8_UNORM coverage under the exact2-target/x4 signature.
Coverage is binary actual geometric support, independent of opacity/emission,
and uses MAX blending independently of color. Static opaque/background0 and
dynamic/optical/all-matter1 preserve the same depth/discard/sample decisions.
One-target legacy/spatial programs and intermediate caches/splats are retained.
The new shader uses existing directory staging; runtime qualification is pending.

`grid_particle_prepare.slang::projectParticleCensus` is capture-only and projects
all original metadata IDs, including offscreen/cached-pack rows, to a 24-byte
stable-ID/original-material/status/native-cell record. Moving rows use Hot64
sourceRow/byte60 enabled; gas uses direct metadata metres. Domain is half-open;
invalid/outside/inactive/explicit-vacuum flags retain every row without clamping.
The ordinary packing/culling entry bodies and their buffer layouts are unchanged.


## Committed-cell native transport analytic experiment

The shared `world_radiance.slang` walker resolves the immutable sparse56-byte
cell overlay through a full-key ordinal table alongside unchanged terrain IDs.
Terrain opaque wins; absorbing sigma/j add without clipping overfill. Partial
coverage is a native entered-face surface event (C*S,1-C), not volume emission.
Secondary events use C*A*the same owner's incident moment with authored S/j
suppressed. Strict partial starts owe no event; unity starts retain full opaque
behavior. Exact hard-backed partial boundaries own a virtual partial face, and
existing transmissive-predecessor hard concave winners remain whole hard surfaces.
Discovery collects every contributing partial key and the terminal hard key in
that SAME walk. The existing qualification query reserved2 enables all-key
collection for bounded analytic evidence; production query geometry is unchanged.
This is a coarse coverage model, not grain geometry or scattering. GPU qualification
and later Game projection are separate gates.


## Immutable committed-Matter projector core

`grid_particle_native.slang` supplies the shared uncropped native-cell/alive/domain
projection used by particle census and optical snapshot; it does not change the
ordinary particle pack entries. Metadata word28 is positive binary32 physical fill,
with zero explicitly unavailable to participating optical input.

`world_matter_project.slang` owns snapshot, stable four-bit radix grouping, full
cell-run reduction, ordinal lookup/ancestor occupancy and changed-cell summary.
Its fixed pass-private record/frame protocols are16/80bytes; output uses the
qualified56-byte optical cell row from `world_matter_types.slang`. Only the40-word
status/bounds/count summary is read back. The qualified native DDA consumes the
same immutable output and checks sparse ancestor occupancy before static-vacuum
skips. The producer is a core checkpoint; Editor/Game scheduling and actual-world
epoch qualification remain separate, unimplemented acceptance work.


### Parallel direct-window refinement

`refineWorldRadianceParallel` uses one64-thread group per receiver. GroupIndex
owns bins `lane+64*k` for four unchanged `worldTraceRaw` calls; masked bins write
zero RGB/status into four256-element shared component arrays (4096 bytes plus
the existing32-byte integer counter scratch). A uniform barrier precedes lane0's
phi0..255 projection using the same local projection helpers as retained
`refineWorldRadiance`/`refineWorldBounce`. No floating reduction or uploaded ABI
changes are involved. Only direct-window page roots select this entry.

### Parallel reflected face moments

`bakeWorldFaceMomentsParallel` assigns one64-thread group to each admitted face,
using GroupIndex bins `lane+64*k`. Inward bins initialize the existing shared RGB
and status arrays without tracing. After one uniform barrier, lane0 accumulates
outward bins in the retained serial entry's original phi order and weighting.
All lanes contribute integer work counters; lane0 alone reports the output row
and status. The pass selects the parallel entry in its existing moment pipeline
slot and bounds moment groups by the device group-count limit. The unchanged
`bakeWorldFaceMoments` entry remains the shared GPU fixture's serial control.
No uploaded ABI, scheduling quota, transport or lifecycle changes are involved.

### Temporal world cascades and mandatory work

`solveWorldTemporalCascade`, `refineWorldTemporal` and retained serial refinement
operate on persistent full-world cascade/CH buffers. Default quality is spacing4
with six levels including refinement. Required parent parities finish before finer
consumers; selected native demand regions expand to complete interpolation support.
Previous completed coefficients supply multibounce feedback and history.

`buildWorldSkipTable` writes certified empty aligned block sizes0/1/2/4/8/16/32 to
an R8UI source-local texture. The optional marcher path preserves the existing DDA
and canonical Matter occupancy contract; a changed source rebuilds its table.

The shared GPU fixture dispatches this same builder before `traceWorldSkipQueries`.
It compares raw signed radiance, RGB transmission, status, segment counts and
entered-face keys against `traceWorldQueries` on identical inputs. The dedicated
entry enables table lookup independently of reflected-light feedback so the
traversal comparison uses the same incident radiance on both paths.

`buildWorldTemporalWork` packs absolute record indices in row order within each admitted sparse rectangle
and writes indirect dispatch arguments. The64-byte temporal constant protocol
carries region, history and work-list controls. Ray entries read the queue count
and absolute indices, retaining direct mapping as a correctness control. Queue
construction preserves every required probe. For optional fine convergence,
`buildWorldConvergenceWork` classifies up to 256 candidates per workgroup into
missing/aged, high-change, and moderate-change bins. It compacts them in row order
within each bin and emits the indirect group count. The pass copies the count
after dispatch and verifies it against actual refined rows at the accepted fence.
The full candidate count remains the predictive GPU cost bound.

Temporal refinement also writes one24-byte probe-state record: two-word source
version, two-word injected sampling time, sample count and normalized squared
coefficient-change score. All27 colour/directional coefficients contribute; scaling
before subtraction keeps finite extreme HDR differences representable. Same-source
samples smooth that score; source replacement resets the count. Failed/unknown
rows have zero version/count. A current sample becomes eligible sooner as its normalized change rises; at 250 ms it is due regardless of score. Diagnostic readback is opt-in only.

`checkWorldTemporalCoverage` uses8x8 groups to count requested supporting probes
whose source version is stale or unknown. It reads the completed coefficient-slot
metadata, including retained rows; only one32-bit count is copied to the CPU. The
pass runs after all selected refinement and its cost stays inside the final batch
timestamp. Version tags alone are not whole-frame deadline evidence.

Temporal coefficient history is reused only when the previous probe is known and
its transport source version matches. Source edits therefore replace primary
lighting without blending stale coefficients; previous-field reflected transport
still permits progressive multibounce settling. Both refinement paths use the
time since the selected probe's previous sample. The setting gives retention
per 1/60 second. A quiet probe with default retention keeps half the previous
variation after one such interval and a quarter after two. Subtracting the
split nanosecond words before converting to float preserves short intervals
across a low-word wrap. Repeated sample times retain the previous smoothed
coefficients exactly when history is enabled.

`solveWorldTemporalCascadeParallel` assigns four adjacent lanes to each
nonterminal interval record, tracing its four parent connectors concurrently.
The owner lane accumulates all sixteen parent contributions in the serial
sample/child order. A64-thread group owns16 records, including masked tail lanes
that still participate in shared-memory barriers. Terminal tracing retains the
serial-per-record kernel; the serial reference also retains the original merges.
Shared connector/parent/weight storage is2816 bytes before group counters.

The final word of `GridWorldRadianceParameters` selects an offscreen material
timing draw. Visible draws set it to zero. During calibration,
`materialSurface` retains the textured contour reflectance, opacity and source
calculation and bypasses world-field/light response. Its result goes only to
the Grid pass's private MSAA timing target.


In temporal Grid terrain shading, the contour selector has already fetched a
4×4 native material stencil. A static opaque surface below a fully occupied
face neighborhood receives only its own emission. The shader checks the four
central cells and their eight exterior face neighbors before sampling textures
or searching for exposed faces. The four diagonal extremities cannot expose a
face. Mixed or incomplete neighborhoods continue through ordinary material and
radiance shading. Geometric coverage, alpha composition, moving surfaces and
the reference field's cut-face response keep their existing paths. The bounded
material timing draw uses the same shortcut so its credited cost tracks the
terrain work that was actually needed.

`worldTerminalSky` samples the authored uniform sky when the temporal hour is
absent. For an application-supplied solar hour, a bounded blue sky and broad
warm directional lobe rotate around the world plane. Solar elevation scales
their intensity, with night going dark. Only coarsest terminal escapes sample
this input. Opaque hits, finite interval misses and secondary-only traces keep
their existing optical semantics. The last word of the 96-byte WorldParameters
block carries the hour plus one; zero selects the authored boundary.

The temporal connector for a child record starts exactly where its incoming
parent connector ended. Four angular children share a near endpoint and fan
out toward their own far endpoints. Level one uses its own direction; the
refinement trace connects the receiver to that exact level-one endpoint. This
keeps one connector per spatial parent and the existing four-child integral.
A 64×64 vacuum room with a one-cell opaque shell checks the entire interior
probe region for daylight leakage. The straight-ray and explicit bounce
reference paths keep their own interval geometry.
