# assets/shaders/

## Responsibility
Slang shader sources for the PPR engine demo. Each `.slang` file is compiled at startup by `engine.shader` (`IShaderService::loadModuleFromFile`) and linked into GPU pipelines by `engine.rhi`. The `Renderer` keeps pipeline-rebuild machinery (`rebuildPipeline_`) for hot-reload, but it is not yet wired to a file watcher (the per-frame `wasReloaded()` check is disabled).

## Design
- **Row-vector / row-major convention**: matrices are row-major (set at the Slang session level in `engine.shader`). Vertex transforms use `mul(float4(position, 1.0), g_frame.m_view_projection)` — the row-vector form — NOT `mul(matrix, vector)`, which would transpose the transform and flip view-space Z (negative W → clipped geometry). This matches `mango::math` / `engine.math` conventions.
- **Frame constants**: a `ConstantBuffer<FrameConstants>` (register b0) carries `m_view`, `m_projection`, `m_view_projection`, `m_inverse_view_projection`, camera position/velocity, and viewport size.
- **Entry points**: marked with `[shader("vertex")]` / `[shader("fragment")]`; HLSL semantics (`POSITION`, `COLOR`, `SV_Position`, `SV_Target`).

## Flow
`IShaderService::loadModuleFromFile(path, name, out)` → `io::mapFile` reads source → wrapped in a `MappedFileBlob` → `ISession::loadModuleFromSource` compiles → module owned by the session until `shutdown()`. `Renderer::rebuildPipeline_()` can rebuild the shader program and pipeline on reload, but no file watch currently triggers it.

## Integration
- Consumed by: `engine.shader` (compilation), `engine.rhi` (pipeline creation), `game` demo.
- Depends on: Slang compiler, `engine.math` matrix layout conventions.

## Key Files
- `triangle.slang` — minimal vertex/fragment pair: passes per-vertex color through, applies `m_view_projection` in row-vector form.
