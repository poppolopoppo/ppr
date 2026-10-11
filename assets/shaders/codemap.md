# assets/shaders/

## Responsibility

Slang shader assets for application render passes. Two live programs: `mesh_bindless.slang` is the
`TrianglePass` static-mesh program, and `grid_tiles.slang` is the `GridPass` debug-grid program (flat
tinted or per-cell quads, one instance per visible chunk-sized tile). The former `triangle.slang`
reference asset was deleted; `TrianglePass` and `GridPass` are the only loaders from this directory.

`grid_visibility.slang` supplies GridPass compute reset and cull entries. It
matches the CPU reference's conservative boxes and frustum planes, atomically
appends at most 1024 visible IDs, and writes one indirect draw record. The tile
vertex entry optionally resolves instances through those IDs. Reset, cull,
barriers and drawing share one encoder and submission.

## Design

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
  `view * projection`, `[0,1]` depth, no transpose/Y flip) on the z = 0 plane; no lighting, texturing,
  or GI inputs. `ConstantBuffer<GridFrame> g_grid_frame` carries only the unjittered `m_view_projection`.
- `grid_tiles.slang` draws six non-indexed verts (two triangles) per tile rect from
  `StructuredBuffer<GridTilePayload> g_tiles` (CPU-mirrored 32 B: rect + material + cell-word base +
  flags + pad); rasterizer culling is off. The per-draw base is entry-point `uniform uint g_tile_base`
  for the same globalParams-overlap reason as the mesh shader.
- `grid_tiles.slang` per-cell lane: when payload flag bit 0 is set, the fragment resolves the cell
  through packed `StructuredBuffer<uint> g_cell_materials` (8192 words per chunk, halfword per cell,
  `0xFFFF` discards); otherwise the payload material is used. Tint is deterministic from the low 3
  material bits (8 dim/bright cube corners), with no palette table or texture.
- `grid_tiles.slang` entry points: `[shader("vertex")] vertexGridMain` and
  `[shader("fragment")] fragmentGridMain`.
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
(`vertexGridMain` + `fragmentGridMain`), binding `g_grid_frame`, `g_tiles`, `g_cell_materials`, and the
`g_tile_base` per draw group.

## Integration

- **Consumers**: `engine.shader` (compile), `engine.rhi` + `TrianglePass`/`GridPass` (pipelines +
  constants), `game` demo.
- **Depends on**: Slang compiler; `engine.math` matrix-layout/handedness conventions (row-major,
  row-vector, left-handed +Z forward, [0,1] depth).
- **Provides**: the runtime bindless mesh shader asset for the scene pass and the grid-tile shader
  asset for the debug-grid pass.
- **Known limitations (documented in-shader, deliberately unfixed)**: the model matrix must be a
  rigid transform or a uniform scale, since directions are rotated with `mul()` rather than the
  inverse-transpose; and `FrameConstants` carries four fields (`m_view`, `m_projection`,
  `m_inverse_view_projection`, `m_viewport_size`) that this shader never reads but that the CPU
  `sizeof == 288` assertion and `uploadFrameConstants_` still require.

## Key Files

- `mesh_bindless.slang` - the live bindless static-mesh vertex/fragment program.
- `grid_tiles.slang` - the live debug-grid tile vertex/fragment program (`vertexGridMain` + `fragmentGridMain`).
