# assets/shaders/

## Responsibility

Slang shader sources for the demo scene pass. Sole file `triangle.slang` draws the per-vertex-colored triangle,
transformed by the frame's view-projection matrix supplied by the engine's `TrianglePass` constant buffer.

## Design

- `FrameConstants` (`register(b0)`): `m_view`, `m_projection`, `m_view_projection`, `m_inverse_view_projection`,
  plus `m_camera_position` and `m_viewport_size` (both `float4`). No lighting/material/time fields — minimal debug
  triangle only.
- Row-vector / row-major convention (matches `mango::math` + Slang session default): vertex does
  `mul(float4(input.position, 1.0), g_frame.m_view_projection)` — NOT `mul(matrix, vector)`, which would transpose
  the transform and flip view-space Z (negative W → clipped). The in-file comment documents this explicitly.
- Entry points `[shader("vertex")] vertexMain` (POSITION/COLOR → SV_Position/COLOR passthrough) and
  `[shader("fragment")] fragmentMain` (color → `float4(color, 1.0)` to SV_Target). No UI shader here — ImGui's is
  embedded in `App.UI.ImGui.cpp`.
- Only `.slang` file in the repo; deployed as `<exe-dir>/shaders/triangle.slang`.

## Flow

`Renderer`/`TrianglePass::initialize` → `IShaderService::loadModuleFromFile(<exe>/shaders/triangle.slang)` →
`io::mapFile` → `MappedFileBlob` → `ISession::loadModuleFromSource` → entry points linked into the triangle render
pipeline; per-frame `FrameConstants` uploaded, vertex shader applies `m_view_projection`.

## Integration

- **Consumers**: `engine.shader` (compile), `engine.rhi` + `TrianglePass` (pipeline + constants), `game` demo.
- **Depends on**: Slang compiler; `engine.math` matrix-layout/handedness conventions (row-major, row-vector,
  left-handed +Z forward, [0,1] depth).
- **Provides**: `triangle.slang` source only.

## Key Files

- `triangle.slang` — vertex/fragment pair with `FrameConstants` and documented row-vector `mul` order.
