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
  NDC-to-framebuffer projection) compiled via `IShaderService::loadModuleFromSource` (`imgui` / `imgui.slang`,
  `vertexMain` + `fragmentMain` linked as `SingleProgram`) — no `.slang` file on disk.
- Construction (`ImGuiService()`): installs `malloc`/`free` via `SetAllocatorFunctions` (TODO: custom scope), wires
  `InputAction` callbacks (`any_digital` on started/triggered/completed, cursor/wheel/sticks/triggers on triggered),
  and builds the `ImGuiInputs` mapping: `any_digital`, `gamepad_left/right_2d`, `gamepad_left/right_trigger_axis`,
  `mouse_2d`, plus `mouse_wheel_axis_x/y` each folded into the wheel action via a small 1D→pair lambda; character
  callback installed on the listener.
- Input routing: seven `InputAction`s (split per device).
  gamepad-button digitals, cursor axis_2d, wheel axis_1d, stick axis_2d, trigger
  axis_1d). `update()` gates per-action consume AFTER `NewFrame()` (last
  frame's widgets, one-frame staleness): keyboard follows `WantCaptureKeyboard`
  (ActiveId/modal/nav — no `AnyWindowFocused` fallback); mouse follows
  `WantCaptureMouseUnlessPopupClose` (hover-split); gamepad follows `NavActive`
  with `NavEnableGamepad` (typing never blocks gamepad). Set flag sinks into
  the UI (`consumed`); cleared flag yields `handled` downstream. `shutdown()`
  resets all consume flags to non-consuming (player default). Character callback drops Ctrl+key control characters,
  gates on `io.WantTextInput`, UTF-8-encodes the codepoint (`hal::native::utf8`) and returns `consumed`/`unhandled`.
  Any-digital dispatches on `trigger.m_code` to `AddKeyEvent` (keyboard via `keyboardKeyToImGuiKey`, gamepad via
  `gamepadButtonToImGuiKey`) or `AddMouseButtonEvent` (via `mouseButtonToImGui`); keyboard path also mirrors
  left/right Ctrl/Shift/Alt/Super into `ImGuiMod_*`. Cursor forwards absolute client pos to `AddMousePosEvent`;
  wheel forwards relative 2D delta to `AddMouseWheelEvent`; sticks fan out per left/right trigger to
  `AddKeyAnalogEvent` L/R-stick directionals; triggers drive `AddKeyAnalogEvent` L2/R2.
- Frame resources: 2-slot vertex/index buffer ring (`FrameResources`, indexed by `m_frame_revision % 2`),
  reallocated on growth (`alignForward` 8192 vtx / 16384 idx, `Upload` `VertexBuffer|CopySource` /
  `IndexBuffer|CopySource`), uploaded per-frame in `uploadDrawData_()` via map/`memcpy`/unmap (no-op when
  `TotalVtx/IdxCount == 0`); font texture built once from `GetTexDataAsRGBA32` (`DeviceLocal` RGBA8 `Texture2D` with
  `ShaderResource|CopyDestination`, `Upload` staging buffer, `copyBufferToTexture` + `setTextureState(ShaderResource)`
  + `submit`/`waitOnHost`, `getDefaultView`, `SetTexID` to the view).
- `initialize(WindowInputContext&, IRhiService&, IShaderService&, priority)` creates the context (`not_supported` when
  null), installs `ErrorCallback` → `PPR_LOG_RAW` with `ConfigErrorRecovery=true`, `EnableAssert=false`,
  `EnableDebugLog=true`, `EnableTooltip=PPR_ENABLE_DEBUG`, sets backend names (`pP_IUIService` / `pP_SlangRHI`,
  `BackendPlatformUserData=this`) and flags (`HasGamepad`, `RendererHasVtxOffset`) and config flags (docking, DPI
  font/viewport scale, gamepad + keyboard nav), seeds `DisplaySize` from `m_framebuffer_size` and
  `DisplayFramebufferScale` from `m_content_scale`, attaches the listener at the given priority, then builds invariant
  input layout/sampler (`POSITION/TEXCOORD RG32Float`, `COLOR RGBA8Unorm`; linear `ClampToEdge` sampler) + shader +
  font. `update(dt, WindowViewport&)` (`not_connected` when no context) refreshes `DeltaTime` (`time::seconds`),
  display size from `viewport.getViewport().getClientRect().m_extent`, framebuffer scale from
  `viewport.getWindow().m_content_scale`, bumps `m_frame_revision`, and calls `NewFrame`. `render(DrawContext)`
  (`not_connected` when no context; no-op when `draw_data` null/invalid) lazily rebuilds the pipeline per
  `RenderPipelineKey` via `createRenderPipeline_` (rejects anything but single color target, no depth, sample count 1;
  SrcAlpha/InvSrcAlpha color blend, One/InvSrcAlpha alpha blend, `CullMode::None`, scissor on, no depth test/write,
  label `render_imgui`), binds `g_scale` (`2/DisplaySize`, Y negated) / `g_offset` (`-1,1`) / font sampler/texture via
  `ShaderCursor` (`broken_pipe` when `bindPipeline` fails), uploads draw data, then issues one indexed draw per command
  with `(ClipRect−DisplayPos) × framebuffer-scale` scissors clamped to the viewport extent (degenerate rects skipped)
  and `UserCallback` passthrough, accumulating global `VtxOffset`/`IdxOffset`. `shutdown()` removes the input listener
  first, releases pipeline/buffers/texture-view/texture/sampler/layout/program, then detaches backend fields
  (`Backend*Name/UserData=null`, `BackendFlags=None`, `SetTexID(nullptr)`), destroys the context, and nulls pointers.
  Helpers `keyboardKeyToImGuiKey` / `gamepadButtonToImGuiKey` / `mouseButtonToImGui` (unknown → `None`/`-1`),
  `framebufferScaleFor` (logical→framebuffer ratio, guarded fallback to 1), and `imGuiDebugPrintf` (`%s` assert +
  `PPR_LOG_RAW`). `getContext()` exposes the raw `ImGuiContext*` so `game/main.cpp` can `SetCurrentContext` +
  `ShowDemoWindow`; destructor asserts context already destroyed and clears mapping/listener bindings.

## Flow

1. `Application::initialize()` → `createImGuiService()` → `initialize(window_input_context, rhi, shader, priority)`:
   context + error/recovery config, backend names/flags + docking/DPI/nav flags, display size/scale seed, shader from
   source string, invariant input layout/sampler, font texture, input-listener attach.
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
  textures), `engine.shader` (from-source compile), `engine.core` (logging, `safe_ptr`, `safe_narrowing`, time),
  `engine.math` (`float2/float4`, `int2`, `uint4`), `imgui` + `imgui_internal` (context, IO, draw data).
- **Provides**: `:ui.imgui` factory + `imVec` conversions only; the class itself is intentionally not exported.

## Key Files

- `App.UI.ImGui.cppm` — `pP::ui::createImGuiService()` factory + `IUIService` forward declaration + `imVec` helpers.
- `App.UI.ImGui.cpp` — `ImGuiService` implementation (initialize/update/render/shutdown, font/upload/pipeline helpers,
  input-to-ImGuiIO translation, embedded shader string).
