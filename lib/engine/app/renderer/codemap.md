# lib/engine/app/renderer

## Responsibility

The `engine.app:renderer` module provides the content-free generic `Renderer` (multi-window surfaces + graphics
queue + validated submission only). Scene content lives in `engine.app:renderer.triangle_pass` (`TrianglePass`);
camera-free RHI-facing submission shapes (`RenderPipelineSignature/Key`, `DrawContext/Callback/Submission`,
`ColorAttachmentOps`, `SurfaceRenderPass`) live header-only in `engine.app:renderer.types`. There is no
`App.Renderer.Types.cpp` — the partition is fully inline in `App.Renderer.Types.cppm`.

## Design

- **Renderer** (`App.Renderer.cppm/.cpp`): config `m_preferred_surface_format` (default Undefined → backend
  preferred), `m_desired_image_count{3}`, `m_enable_vsync{true}`; state `FlatMap<WindowHandle, SurfaceRecord>` +
  `m_graphics_queue` + `safe_ptr<IRhiService>`. `SurfaceRecord` holds `ISurface`, last `m_extent`, `m_configured`.
- `initialize(rhi_service)` grabs the Graphics `ICommandQueue` only; `shutdown()` retain-first-error teardown:
  `waitOnHost`, unconfigure every configured surface, clear map/queue/service refs. `waitOnHost()` forwards when
  a queue exists, else success.
- `render(description, render_pass, draws)`: rejects uninitialized queue/service (`not_connected`) and empty/null
  attachment descriptors (`invalid_argument`); `inspectAttachment_` resolves each color/depth view to
  format + mip-aware extent + sample count (null/missing/out-of-range/Undefined/0-sample → `invalid_argument`);
  `validateMatchingAttachment_` requires identical extent + sample count across all attachments; resolve targets
  require MSAA source (>1x), 1x resolve, same extent + format. Builds `RenderPipelineKey` signature, creates a
  command encoder, `beginRenderPass` (null → `io_error`), pushes a purple debug group, derives default
  viewport/scissor from the reference extent, then per `DrawSubmission` inserts a pink debug marker, applies
  `m_viewport/m_scissor` or defaults via `RenderState`, and invokes `m_encode_draws(DrawContext{device, pass,
  pipeline_key, viewport, scissor, target_extent})`; finishes and submits one command buffer.
- `renderToTexture(target, draws, options)`: builds a single color attachment from `target.getDefaultView()` with
  `applyColorAttachmentOps_` (load/store/clear color) and forwards to `render()` with the texture label.
  Offscreen callers must `waitOnHost()` before readback.
- `renderAndPresent(window, draws, surface_pass)`: rejects uninitialized backend (`not_connected`) and null window
  handle (`no_such_device_or_address`); lazily `createWindowSurface_` on first use, otherwise
  `resizeWindowSurface_` when `m_framebuffer_size` drifts; unconfigured record (minimized/zero extent) returns
  `resource_unavailable_try_again` without drawing; otherwise `acquireNextImage`, assembles surface color + `m_additional_colors` +
  optional `m_depth_stencil`, `render()` under the window-title debug group, then `present()`, retain-first-error.
- `createWindowSurface_` validates out-param/service/native/handle, `createSurface(fromHwnd(native))`,
  `resizeWindowSurface_` to the current framebuffer size, `insert_or_assign`. `resizeWindowSurface_`: zero/negative
  extent unconfigures (keeps the record); otherwise waits when already configured, then `configure()` with
  width/height + preferred format + image count + vsync. `destroyWindowSurface(window)` rejects null handle
  (`invalid_argument`) and forwards to `destroyWindowSurface_(handle)` (unknown handle → `invalid_argument`;
  else move-out, erase, unconfigure).
- **Types** (`App.Renderer.Types.cppm`, header-only): `RenderPipelineSignature{color_formats span,
  depth_stencil_format optional, sample_count}` with `operator==` + `hashValue` combine, memoized as
  `RenderPipelineKey`; `DrawContext{IDevice&, IRenderPassEncoder&, pipeline_key, viewport, scissor,
  target_extent}`;
  `DrawCallback = function_ref<error_code(DrawContext)>`; `TDrawable` concept (`render(DrawContext) →
  error_code`); `DrawSubmission{description, encode_draws, optional viewport/scissor}` with direct and drawable
  (`typeid` label + `nontype<&T::render>`) constructors — retains nothing after `render()` returns;
  `ColorAttachmentOps{clear_color (0.1,0.1,0.2,1), Clear/Store}`; `SurfaceRenderPass{surface_color,
  additional_colors span, depth_stencil optional}`.
- **TrianglePass** (`App.Renderer.TrianglePass.cppm/.cpp`): pass-owned GPU caches +
  shared sampler + narrow upload APIs (`uploadMesh/uploadTexture/packMaterial/submitInstance`)
  +   per-instance list + pipeline-variant map (+ kept `CameraSnapshot`). Binds the scalar-handle
  `mesh_bindless.slang` program (StructuredBuffer fetch, no fixed-function geometry) and encodes
  per-instance pushes/resolved slots/descriptors with fail-closed handle resolution.
  `FrameConstants` keeps its `sizeof == 288` HLSL mirror assert (4×float4x4 + 2×float4).
- **GpuCaches** (`App.Renderer.GpuCaches.cppm/.cpp`): `TriangleBagCache` (vertex-type-agnostic
  bump buckets, stable `TriangleBagRange`), `BindlessTextureCache` (content-hash dedup +
  refcount + pin-while-held), `BindlessMaterialCache` (stable `GpuMaterial` slots, 80 B stride),
  `buildGpuMaterial` pack mapping, pipeline-variant key. Render-thread confined; shutdown
  caches → sampler → pipelines before renderer `waitOnHost` (§2.4).
- `initialize(rhi, shader, content_dir)`: bindless program/layout + `caches.initialize(device)`
  with reverse-order rollback; `createShaderProgram_` loads `mesh_bindless.slang`;
  `createRenderPipeline_` keys opaque/mask variants (blend rejected); `update(dt, camera_view)`
  caches the snapshot; `render(ctx)` encodes per instance (§6).

## Flow

1. Startup → `renderer.initialize(rhi_service)` (graphics queue only); first `renderAndPresent(window, …)`
   lazily creates + configures the window surface from `Window::m_framebuffer_size/m_native`
2. Per-frame → stack `DrawSubmission`s (named lambdas/`function_ref` or `TDrawable` refs) → `renderAndPresent`
   (resize on drift → acquire → `render` → present); `TrianglePass::update(snapshot)` then `TrianglePass::render`
   inside a submission's encode callback
3. Offscreen/tests → `renderToTexture(target, …)` → `waitOnHost()` → readback
4. On minimize/resize-to-zero → surface unconfigures but the record stays; next non-zero frame reconfigures
5. Shutdown → `triangle.shutdown()` → `destroyWindowSurface(window)` per window → `renderer.shutdown()`
   (wait, unconfigure all, release)

## Integration

- **Consumers**: owning application shell (owns `Renderer` + `TrianglePass`, drives update/render/shutdown)
- **Depends on**: `engine.core`, `engine.math`, `engine.rhi` (devices, queues, surfaces, passes, pipelines),
  `engine.shader` (TrianglePass program load), `engine.image` + `engine.mesh` (cache upload types only),
  `:service.window` + `:window.handle` (Window resolves surfaces),
  `:scene.camera` (TrianglePass `CameraSnapshot` only — never a mutable `Camera`)
- **Provides**: `engine.app:renderer`, `engine.app:renderer.triangle_pass`, `engine.app:renderer.types`
  (camera-free boundary: passes consume `DrawContext`/snapshot data while drawing)

## Key Files

- `App.Renderer.cppm` — generic `Renderer` declaration (config, multi-surface registry, render/renderToTexture/renderAndPresent/waitOnHost/destroyWindowSurface)
- `App.Renderer.cpp` — Renderer implementations (attachment inspection/validation, encode/submit, surface create/resize/destroy, retain-first-error shutdown)
- `App.Renderer.TrianglePass.cppm` — `TrianglePass` declaration (FrameConstants layout, snapshot cache, caches, pipeline helpers)
- `App.Renderer.TrianglePass.cpp` — TrianglePass implementations (invariant state, shader program, variant pipelines, §6 encode, frame-constant upload)
- `App.Renderer.GpuCaches.cppm` — pass-owned cache vocabulary (handles, `TriangleBagRange`, `GpuMaterial`, caches)
- `App.Renderer.GpuCaches.cpp` — cache implementations (uploads, dedup, pack, teardown)
- `App.Renderer.Types.cppm` — boundary types (`RenderPipelineSignature/Key`, `DrawContext/Callback/Submission`, `ColorAttachmentOps`, `SurfaceRenderPass`); header-only, no matching `.cpp`
