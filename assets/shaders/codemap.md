# assets/shaders/

## Responsibility

Slang shader assets for application render passes. `mesh_bindless.slang` is the live `TrianglePass` program;
`triangle.slang` remains a minimal colored-triangle reference shader.

## Design

- Both shaders use PPR's row-major, row-vector convention: transforms call `mul(vector, matrix)`, never
  `mul(matrix, vector)`. View-projection remains `view * projection`.
- `mesh_bindless.slang`: pure `StructuredBuffer` vertex/index/material fetch driven by `SV_VertexID`; one
  `ConstantBuffer<FrameConstants>`; per-instance model/push values as vertex entry-point `uniform` parameters;
  scalar texture/sampler handles as fragment entry-point parameters; opaque/mask material shading with ORM
  channel mapping. CPU mirrors and cursor bindings live in `TrianglePass`, `GpuCaches`, and `Mesh.Types`.
- `triangle.slang`: minimal POSITION/COLOR vertex input and color fragment output using the same 288-byte
  `FrameConstants` layout. It is a reference asset, not the shader currently loaded by `TrianglePass`.
- Both define `[shader("vertex")] vertexMain` and `[shader("fragment")] fragmentMain`.
- The ImGui shader remains embedded in `App.UI.ImGui.cpp`, not under this asset directory.
- CMake stages the shader directory beside application and relevant test executables.

## Flow

`TrianglePass::initialize` -> `IShaderService::loadModuleFromFile(<content>/shaders/mesh_bindless.slang)` ->
`io::mapFile` -> `MappedFileBlob` -> `ISession::loadModuleFromSource` -> `vertexMain` + `fragmentMain` linked into
the pass-owned program and pipeline variants; `ShaderCursor` binds frame data, structured buffers, descriptor
handles, and per-instance values before each non-indexed draw.

## Integration

- **Consumers**: `engine.shader` (compile), `engine.rhi` + `TrianglePass` (pipeline + constants), `game` demo.
- **Depends on**: Slang compiler; `engine.math` matrix-layout/handedness conventions (row-major, row-vector,
  left-handed +Z forward, [0,1] depth).
- **Provides**: runtime shader assets for the scene pass plus the minimal triangle reference.

## Key Files

- `mesh_bindless.slang` - live bindless static-mesh vertex/fragment program.
- `triangle.slang` - minimal colored-triangle reference with the canonical matrix order.
