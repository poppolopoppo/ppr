# assets/shaders/

`pbmpm/solver.slang` and `pbmpm/types.slang` are the direct pinned PB-MPM
compute port owned by `engine.matter`. Their source attribution, six-entry-point
schedule plus publication validator, explicit scalar CPU/GPU ABI and admitted scope are recorded in
`pbmpm/README.md`; `pbmpm/LICENSE.md` retains the upstream BSD-3-Clause notice.

## Responsibility

Slang shader assets for application render passes. Three live programs: `mesh_bindless.slang` is the
`TrianglePass` static-mesh program, `grid_tiles.slang` is the `GridPass` debug-grid program (flat
tinted or per-cell quads, one instance per visible chunk-sized tile), and `retro_present.slang` is the
`RetroPass` low-resolution present program. The former `triangle.slang`
reference asset was deleted; `TrianglePass`, `GridPass`, and `RetroPass` are the only loaders from this directory.

`grid_visibility.slang` supplies GridPass compute reset and cull entries. It
matches the CPU reference's conservative boxes and frustum planes, atomically
appends at most 1024 visible IDs, and writes one indirect draw record. The tile
vertex entry optionally resolves instances through those IDs. Reset, cull,
barriers and drawing share one encoder and submission.

## Design

`grid_particle_prepare.slang` is GridPass-owned resident-particle presentation,
with reflected numerical64/metadata64/packed32/frame144 byte protocols. It
requires an explicitly admitted D3D12 FP64 device for exact metric conversion.
Stable ten-bit chunk radix partitions preserve metadata ID order without
reordering solver storage; a bounded hierarchical scan produces ordered visible
IDs and indirect arguments entirely on GPU. `grid_tiles.slang` shares one
particle geometry helper between direct and indirect vertex entries.

- `mesh_bindless.slang` uses PPR's row-major, row-vector convention: transforms call
  `mul(vector, matrix)`, never `mul(matrix, vector)`. View-projection remains `view * projection`.
- Pure `StructuredBuffer` fetch driven by `SV_VertexID`: no fixed-function geometry bindings, so the
  pass needs no input layout.
- One `ConstantBuffer<FrameConstants>` (`g_frame`) plus structured buffers for vertices, indices,
  materials, and per-instance payloads. There is no per-draw `uniform` model/push entry-point
  parameter: the deleted `vertexMain`/`PushScalars` seam was replaced by the
  `StructuredBuffer<InstancePayload> g_payloads` element read by `vertexIndirectMain`.
- One module-scope ordinary global is forbidden here: it makes Slang emit a
  `globalParams` CBV that overlaps `g_frame`, and `D3D12SerializeRootSignature`
  then fails program creation. Per-draw ordinary data therefore arrives as an
  entry-point `uniform` parameter, like the deleted `PushScalars` seam.
- The one per-draw uniform is `uniform uint g_payload_base` on `vertexIndirectMain`.
  `SV_InstanceID` is draw-local on D3D12, SPIR-V, and Metal, so `startInstanceLocation`
  never reaches the shader; the base carries the group's first payload index instead.
  C++ binds it with the same flat `shader_cursor["g_payload_base"].setData(...)` used
  for the former `g_model`/`g_push` entry-point uniforms — globals and entry-point
  `uniform` params resolve through one `rhi::ShaderCursor(shader_object)`. See the
  `slang-shader-developer` skill's "Current bindless mesh seam" for the full rationale.
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
  `ConstantBuffer<GridFrame> g_grid_frame` carries the unjittered matrix, visibility
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
- `retro_present.slang` is the `RetroPass` present program: one fullscreen triangle samples the
  low-resolution scene with clamped pixel-center nearest taps (`SampleLevel 0`), converts linear→sRGB,
  applies a 4×4 Bayer threshold scaled by `m_dither_strength`, selects the nearest DawnBringer-32 entry
  by luma-weighted squared distance in sRGB, and converts the selected color back to linear. Its
  `ConstantBuffer<RetroParameters>` is 528 B (32 float4 sRGB entries, float2 source extent, float dither
  strength, uint reserved) mirrored by `RetroPass`; `uniform SamplerState.Handle g_sampler` is the only
  entry-point ordinary-data parameter. Entry points: `vertexRetroPresent` + `fragmentRetroPresent`.
- CPU mirrors and cursor bindings live in `TrianglePass`, `GpuCaches`, and `Mesh.Types`.
- The ImGui shader remains embedded in `App.UI.ImGui.cpp`, not under this asset directory.
- CMake stages the whole shader directory beside application and relevant test executables, so
  adding or deleting a `.slang` file needs no build-system change.

## Flow

`TrianglePass::initialize` -> `IShaderService::loadModuleFromFile(<content_dir>/shaders/mesh_bindless.slang)`
-> `io::mapFile` -> `MappedFileBlob` -> `ISession::loadModuleFromSource` -> `vertexIndirectMain` +
`fragmentMain` linked into the pass-owned program and pipeline variants; `ShaderCursor` binds frame
data, the structured buffers, the `g_payload_base` per draw group, and the descriptor heap before
each draw. `GridPass::initialize` follows the same chain for `<content_dir>/shaders/grid_tiles.slang`
(`vertexGridMain` + `fragmentGridMain`), binding frame constants, tile/cell and
contour/diagnostic buffers, and the bindless texture heap/shared sampler. `RetroPass::initialize`
loads `<content_dir>/shaders/retro_present.slang` (`vertexRetroPresent` + `fragmentRetroPresent`),
then binds the 528-byte palette/extent constant, the selected scene view, and one sampler before each
fullscreen present.

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
- `retro_present.slang` - the live retro present program (`vertexRetroPresent` + `fragmentRetroPresent`).

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
ErikSom859c449 fork: split64/32 particle rows, GPU buckets/indirect fused stages,
explicit workgroup zeroing and guarded signed reductions. It is consumed only
by `app/matter_fork`, comparing frozen actual-WGSL words. Production GpuSolver shares `fork_core.slang` arithmetic and uses count/allocate/insert
plus two indirect fused stages at K2. Its production-path oracle leaf observes
eight post-integration stages and exact semantic restart. Circle presentation
remains a separate implementation gate. Current evidence/limitations live under
`out/diagnostics/pbmpm-recovery-20261022/native-batch1/`.
