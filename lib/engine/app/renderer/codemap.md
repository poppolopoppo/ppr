# lib/engine/app/renderer

## Responsibility

The `engine.app:renderer` module provides the content-free generic `Renderer` (surfaces + graphics queue +
submission only). Scene content lives in `engine.app:renderer.triangle_pass` (`TrianglePass`); submission shapes
(`RenderView`/`DrawSubmission`/`ColorPassOptions`/`SceneView`) live in `engine.app:renderer.types`.

## Design

- **Renderer** class with `initialize(IRhiService&)` — grabs the graphics queue only. Per-window swap chains are
  owned as `FlatMap<WindowHandle, SurfaceRecord>` via `createWindowSurface` / `destroyWindowSurface` /
  `resizeWindowSurface` / `getWindowSurfaceFormat` (unknown handle → `Format::Undefined`; minimized/unconfigured →
  success no-op).
- **renderAndPresent(handle, submissions, options)** — lookup → acquire → `submitToTarget_` → present.
  **submitToTexture(target, submissions, options)** — same encode/submit without present or wait; the caller must
  `waitForIdle()` before readback.
- **encodeDraws_** — one color pass; per submission binds viewport/scissor via `RenderState` and invokes the borrowed
  `DrawCallback`. Submissions retain nothing after return.
- **TrianglePass** — owns triangle shader/pipeline/vertex buffer/root object/frame cursor; `draw()` rebuilds the
  pipeline on target format/sample mismatch and uploads snapshot-fed frame constants (no velocity field; viewport size
  from `SceneView::m_render_view`, never mutable `Camera`).
- **shutdown()** — retain-first-error teardown: wait, unconfigure all surfaces, release queue/service refs.
- **Moves**: viewport geometry moved out to `engine.app:window.viewport` (`App.Viewport.cpp/.cppm` deleted;
  `makeRenderView` in `:renderer.types` now takes `const Viewport &`); camera state moved out to
  `engine.app:scene.camera` (`lib/engine/app/camera/` deleted, `TrianglePass` consumes only `CameraSnapshot`).

## Flow

1. `Application::initialize()` → `renderer->initialize(rhi_service)` + `createWindowSurface(window_service, window)`
2. Per-frame → stack `DrawSubmission`s (named lambdas + `function_ref`) → `renderAndPresent(handle, ...)`
3. Offscreen/tests → `submitToTexture(target, ...)` → `waitForIdle()` → readback
4. On resize → `resizeWindowSurface(handle, size)` (zero size unconfigures, keeps the record)
5. `Application::shutdown()` → `triangle.shutdown()` → `destroyWindowSurface` → `renderer.shutdown()`

## Integration

- **Consumers**: `Application` (primary — owns `Renderer` + `TrianglePass`)
- **Depends on**: `engine.core`, `engine.math`, `engine.rhi`, `engine.shader`, `engine.app:renderer.types`,
  `engine.app:scene.camera` (TrianglePass snapshot), `engine.app:service.window` + `:window.handle` (surfaces)
- **Provides**: `engine.app:renderer`, `engine.app:renderer.triangle_pass`, `engine.app:renderer.types`

## Key Files

- `App.Renderer.cppm` — generic `Renderer` declaration (multi-surface registry, renderAndPresent/submitToTexture)
- `App.Renderer.cpp` — Renderer implementations (configure/submit/encode/present, retain-first-error shutdown)
- `App.Renderer.TrianglePass.cppm` — `TrianglePass` declaration (content-owned pass, velocity-free FrameConstants)
- `App.Renderer.TrianglePass.cpp` — TrianglePass implementations (program/layout/buffer/pipeline, snapshot upload)
- `App.Renderer.Types.cppm` — boundary types (`ColorTargetInfo`/`RenderView`/`DrawContext`/`DrawCallback`/
  `DrawSubmission`/`ColorPassOptions`/`SceneView`) + `makeRenderView`
- `App.Renderer.Types.cpp` — `makeRenderView` (window-local translate, DPI scale, clip, empty → nullopt)
