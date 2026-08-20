# lib/engine/app/renderer

## Responsibility
The `engine.app:renderer` module provides the `Renderer` class responsible for multi-viewport render submission to the RHI. It manages the graphics pipeline (shader program, input layout, vertex buffers), handles surface creation and resize, and renders `ViewportEntry` bundles containing a render pipeline, viewport, scissor rect, and a draw callback. The renderer supports both a backward-compat single-overlay render mode and a multi-viewport mode where scene and UI are rendered to separate viewports.

## Design
- **Renderer** class with `initialize(IRhiService&, IWindowService&, Window)` — sets up the RHI pipeline: creates device queue, surface from native window, loads shader module, creates shader program, input layout, vertex buffer, and render pipeline. Also resolves the frame cursor (`g_frame` constant buffer) that shaders use to access per-frame constants.
- **Renderer::ViewportEntry** = `pP::ViewportEntry` (from `engine.app:viewport`) — bundle of `pipeline`, `viewport`, `scissor`, and `draw` (function_ref). Two entries are typically used: one for the scene viewport, one for UI overlay.
- **Renderer::FrameConstants** — persistent struct (static_assert 304 bytes = 4×float4x4 + 3×float4) holding `view`, `projection`, `view_projection`, `inverse_view_projection`, `camera_position`, `camera_velocity`, `viewport_size`. Mapped via `m_frame_cursor` into the root shader object each frame. Cached per camera to avoid re-upload when camera hasn't changed.
- **Two render modes**: 
  - `render(std::span<const ViewportEntry>)` — multi-viewport: acquires back-buffer, renders each viewport entry via `renderInto_()`, then presents. This is the primary mode.
  - `render(std::optional<OverlayCallback>)` — backward-compat: renders a single overlay callback into the full framebuffer, or falls through to simple buffer swap if no overlay.
- **onResize(int2)** — reconfigures the swap chain surface when the window framebuffer size changes; preserves pipeline state.
- **shutdown()** — releases all RHI resources (pipeline, buffers, program, surface, queue) in reverse order of creation.
- **renderInto(rhi::ITexture, span<const ViewportEntry>)** — renders to an arbitrary offscreen target, waiting for GPU completion before returning (for readback scenarios).
- Camera data is provided via the `Camera` type (from `engine.app:camera`); the renderer does not own camera logic — it receives a `Camera*` or `const Camera&` from the draw callback's captured environment.

## Flow
1. `Application::initialize()` → `renderer->initialize(rhi_service, cached_window_service, main_window, content_dir)` — sets up RHI pipeline
2. Per-frame `Application::render()` → `m_renderer->render(viewports)` where `viewports = [scene_entry, ui_entry]`:
   a. Acquire next back-buffer image via `m_surface->acquireNextImage()`
   b. Call `renderInto_()` which submits a command encoder + render pass, rendering each viewport entry's draw callback
   c. Call `m_surface->present()` to display the image
3. On window resize: `Application::onWindowResized_()` → `m_renderer->onResize(new_fb_size)` → reconfigures swap chain
4. Shader hot-reload: if shader file changes, `m_triangle_shader.wasReloaded()` triggers `rebuildPipeline_()` to recreate the shader program and input layout
5. `Application::shutdown()` → `m_renderer->shutdown()` → releases all RHI resources

## Integration
- **Consumers**: `Application` (primary — creates and uses Renderer), `Camera` (provides view/projection matrices passed to renderer via draw callbacks), `RHI` (indirectly — Renderer creates device resources but does not own the RHI singleton)
- **Depends on**: `engine.core`, `engine.math` (float2, float4x4, viewport/size operations), `engine.rhi` (IRhiService, IRenderPipeline, IBuffer, ITexture, Viewport, ScissorRect, Format), `engine.shader` (IShaderService, ModuleHandle, loadModuleFromFile), `engine.app:viewport` (ViewportEntry, ViewportConfig), `engine.app:camera` (Camera type for draw callbacks)
- **Provides**: `engine.app:renderer` module namespace with `Renderer` class, `ViewportEntry` (via re-export from `:viewport`), `FrameConstants` struct
- **Used by**: `Application` (`m_renderer` member, `render()` and `render()` calls), draw lambdas in `Application::render()` (scene_draw and ui_draw closures capture `m_renderer.get()` and `m_scene_services`/`m_ui_services`)

## Key Files
- `App.Renderer.cppm` — `Renderer` class declaration (initialize, render(optional<OverlayCallback>), render(span<const ViewportEntry>), drawTriangle, getSurfaceFormat, onResize, shutdown, FrameConstants struct)
- `App.Renderer.cpp` — Renderer method implementations (initialize: surface, shader, pipeline, input layout, vertex buffer; renderInto_: command encoder + render pass + submit; render: acquire/present; onResize: reconfig surface; shutdown: release resources; drawTriangle with camera; frame cursor resolve)
- `App.Viewport.cppm` — `ViewportConfig` struct, `ViewportEntry` struct (draw function_ref, pipeline/scissor/viewport)
- `App.Viewport.cpp` — empty implementation partition
- `App.Renderer.cppm` also forward-declares `Camera` from `engine.app:camera`