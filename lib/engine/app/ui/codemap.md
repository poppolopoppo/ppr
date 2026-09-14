# lib/engine/app/ui

## Responsibility

ImGui overlay layer implementing `IUIService` (`engine.app:service.ui`) on top of the RHI swap chain. Owns the ImGui
context, font atlas texture, double-buffered draw buffers, and the dedicated overlay pipeline; routes engine input
events into ImGui IO and renders ImGui draw data as a second viewport entry on top of the 3D scene.

## Design

- Interface `pP::ui::createImGuiService()` (`App.UI.ImGui.cppm`, `:ui.imgui`) returns `unique_ptr<IUIService>`;
  full `ImGuiService` class is file-local in `App.UI.ImGui.cpp` (`module engine.app;` + `import :ui.imgui`); `imVec`
  helpers convert `float2/float4` ↔ `ImVec2/ImVec4`.
- Embedded shader: `kImGuiShader` source string (pos/uv/col vertex → textured fragment with `g_scale`/`g_offset`
  NDC-to-framebuffer projection) compiled via `IShaderService::loadModuleFromSource` — no `.slang` file on disk.
- Input routing: five `InputAction`s (`ImGuiAnyDigital` digital, `ImGuiMouseCursor` axis_2d, `ImGuiMouseWheel` axis_1d,
  `ImGuiGamepadStick` axis_2d, `ImGuiGamepadTrigger` axis_1d) in the `ImGuiInputs` mapping bound to `any_digital`,
  both gamepad sticks/triggers, `mouse_2d`, and wheel X/Y (each wheel axis shuffled into a 2D `{x,0}`/`{0,y}` value);
  character callback gates on `io.WantTextInput`, UTF-8-encodes the codepoint and returns `consumed`/`unhandled`.
  Any-digital dispatches on `trigger.m_code` to `AddKeyEvent` (keyboard/gamepad) or `AddMouseButtonEvent`; cursor
  forwards absolute client pos to `AddMousePosEvent`; wheel currently forwards its 2D delta to `AddMousePosEvent`;
  sticks fan out to `AddKeyAnalogEvent` L/R-stick directionals; triggers drive `AddKeyAnalogEvent` L2/R2.
- Frame resources: 2-slot vertex/index buffer ring (`FrameResources`, indexed by `m_frame_revision % 2`),
  reallocated on growth (`alignForward` 8192 vtx / 16384 idx), uploaded per-frame in `uploadDrawData_()` via
  map/`memcpy`/unmap; font texture built once from ImGui's rasterized atlas (staging upload + state transition +
  `waitOnHost`, `SetTexID` to the view).
- `initialize(WindowInputContext&, IRhiService&, IShaderService&, priority)` creates the context, routes recoverable
  errors to `PPR_LOG_RAW` (assert recovery off), sets backend names/flags (`HasGamepad`, `RendererHasVtxOffset`) and
  config flags (docking, DPI font/viewport scale, gamepad + keyboard nav), seeds display size/scale from the window,
  attaches the listener at the given priority, then builds invariant state + shader + font. `update(dt, WindowViewport&)`
  refreshes `DeltaTime`, client-rect display size and content scale, bumps `m_frame_revision`, and calls `NewFrame`.
  `render(DrawContext)` lazily rebuilds the pipeline per `RenderPipelineKey` (single color target, no depth,
  single-sample, SrcAlpha/InvSrcAlpha blend), binds `g_scale`/`g_offset`/font sampler/texture, then issues one
  indexed draw per command with `ClipRect−DisplayPos` × framebuffer-scale scissors and `UserCallback` passthrough.
  `shutdown()` detaches backend fields/`TexID`, destroys the context, releases pipeline/buffers/texture/layout, and
  removes the input listener. `getContext()` exposes the raw `ImGuiContext*` so `game/main.cpp` can `SetCurrentContext`
  + `ShowDemoWindow`.

## Flow

1. `Application::initialize()` → `createImGuiService()` → `initialize(window_input_context, rhi, shader, priority)`:
   context + error callback, shader from source string, invariant input layout/sampler, font texture, input-listener attach.
2. Per-frame `update(dt, viewport)`: display size from the client rect, framebuffer scale from content scale, delta
   time, routed input → `ImGui::NewFrame()` (game code then records widgets, e.g. demo window).
3. Per-frame `render(draw_context)` → `ImGui::Render()`, lazy pipeline rebind on key change, `g_scale`/`g_offset`
   cursor setup, `uploadDrawData_`, scissored indexed draws over the scene pass.
4. `shutdown()` → listener detach, pipeline/buffer/texture release, backend-field clear, context destroy, null pointers.

## Integration

- **Consumers**: `Application` (creates/owns `m_ui_service`, calls `initialize/update/render/shutdown`), `game/main.cpp`
  (debug-only `ShowDemoWindow` via `getUiServices().get<IUIService>()` + `getContext()`).
- **Depends on**: `:service.ui` (contract), `:service.input/window` (events, size), `:input.key/device/listener`
  (actions, mappings, contexts), `:window.handle/viewport` (window + viewport inputs), `engine.rhi` (pipeline/buffers/
  textures), `engine.shader` (from-source compile), `imgui` + `imgui_internal` (context, IO, draw data).
- **Provides**: `:ui.imgui` factory + `imVec` conversions only; the class itself is intentionally not exported.

## Key Files

- `App.UI.ImGui.cppm` — `pP::ui::createImGuiService()` factory + `IUIService` forward declaration + `imVec` helpers.
- `App.UI.ImGui.cpp` — `ImGuiService` implementation (initialize/update/render/shutdown, font/upload/pipeline helpers,
  input-to-ImGuiIO translation, embedded shader string).
