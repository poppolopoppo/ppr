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
reference asset was deleted. Each pass owns its shader program, and diagnostic
fixtures load their qualification assets directly.

`grid_visibility.slang` supplies GridPass compute reset and cull entries. It
matches the CPU reference's conservative boxes and frustum planes, atomically
appends at most 1024 visible IDs, and writes one indirect draw record. The tile
vertex entry optionally resolves instances through those IDs. Reset, cull,
barriers and drawing share one encoder and submission.


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


