# lib/engine/app/ui

## Responsibility

ImGui overlay layer implementing `IUIService` (`engine.app:service.ui`) on top of the RHI swap chain. Owns the ImGui
context, font atlas texture, double-buffered draw buffers, and the dedicated overlay pipeline; routes engine input
events into ImGui IO and renders ImGui draw data as a second viewport entry on top of the 3D scene.

## Design

- Interface `pP::ui::createImGuiService()` (`App.UI.ImGui.cppm`, `:ui.imgui`) returns `unique_ptr<IUIService>`;
  full `ImGuiService` class is file-local in `App.UI.ImGui.cpp` (`module engine.app;` + `import :ui.imgui`).
- Embedded shader: `kImGuiShader` source string (pos/uv/col vertex → textured fragment with `g_scale`/`g_offset`
  NDC-to-framebuffer projection) compiled via `IShaderService::loadModuleFromSource` — no `.slang` file on disk.
- Input routing: raw key callback + pushed `InputListener` translate engine keyboard/mouse/char/wheel/modifier events
  into `ImGuiIO` (`AddKeyEvent`, `AddMousePosEvent`, …); ImGui listener sits first so the overlay gets events before
  the scene controller.
- Frame resources: 2-slot vertex/index buffer ring (`FrameResources`), reallocated on growth, uploaded per-frame in
  `uploadDrawData_()`; font texture built once from ImGui's rasterized atlas.
- `getContext()` exposes the raw `ImGuiContext*` so `game/main.cpp` can `SetCurrentContext` + `ShowDemoWindow`.

## Flow

1. `Application::initialize()` → `createImGuiService()` → `initialize(rhi, window_svc, input_svc, window, format)`:
   context + error callback, shader from source string, input layout, font texture, pipeline, input-listener push.
2. Per-frame `update()` → `newFrame(dt)`: display size from framebuffer, delta time, routed input → `ImGui::NewFrame()`
   (game code then records widgets, e.g. demo window).
3. Per-frame `render()` → second `DrawSubmission` → `renderOverlay(pass, fb_size)`: `ImGui::Render()`, bind pipeline +
   font texture/sampler, set `g_scale`/`g_offset`, draw indexed primitives.
4. `shutdown()` → queue wait, release buffers/texture/pipeline, pop input listener, destroy context, null pointers.
   `onResize()` updates the cached framebuffer size.

## Integration

- **Consumers**: `Application` (creates/owns `m_ui_service`, calls `newFrame/renderOverlay/shutdown`), `game/main.cpp`
  (debug-only `ShowDemoWindow` via `getUiServices().get<IUIService>()`).
- **Depends on**: `:service.ui` (contract), `:service.input/window` (events, size), `engine.rhi` (pipeline/buffers/
  textures), `engine.shader` (from-source compile), `imgui` + `imgui_internal` (context, IO, draw data).
- **Provides**: `:ui.imgui` factory only; the class itself is intentionally not exported.

## Key Files

- `App.UI.ImGui.cppm` — `pP::ui::createImGuiService()` factory + `IUIService` forward declaration.
- `App.UI.ImGui.cpp` — `ImGuiService` implementation (init/newFrame/renderOverlay/shutdown, font/upload helpers,
  key translation, embedded shader string).
