# lib/engine/app/ui

## Responsibility
The `engine.app:ui` module provides the user interface layer built on top of ImGui, implementing the `IUIService` interface for integration with the engine's render pipeline and input system. It handles ImGui context creation, font atlas generation, input routing (keyboard/mouse events to ImGui), and ImGui frame rendering into the engine's RHI swap chain. The UI layer is designed to be a secondary viewport rendered on top of the scene, with its own viewport entry and draw callback.

## Design
- **IUIService** interface (in `engine.app:service.ui`) — declares `initialize(IRhiService, IWindowService, IInputService, Window, Format)`, `shutdown()`, `newFrame(TimeSpan)`, `renderOverlay(rhi::IRenderPassEncoder, float2)`, `onResize(int2)`, `getContext()`
- **ImGuiService** final class implements `IUIService` — owns an ImGui context (`ImGuiContext*`), a render pipeline, input layout, font texture, and frame resources (double-buffered vertex/index buffers)
- **Input routing**: `ImGuiService::newFrame()` routes raw keyboard/mouse events from `IInputService` into ImGui's IO state (`AddKeyEvent`, `AddMousePosEvent`, `AddMouseWheelEvent`, `AddInputCharacter`, modifier keys). Also has a raw key callback registered with the input service to translate engine input events into ImGui events
- **Frame resources**: Double-buffered (`kFrameCount = 2`) vertex/index buffers uploaded each frame via `uploadDrawData_()` — maps buffers, copies ImDrawVert/ImDrawIdx data, unmaps
- **Render pass**: Renders ImGui draw data into the swap chain color target using a dedicated pipeline. Sets `g_scale` and `g_offset` constants for projection from NDC to framebuffer space; binds font texture and sampler
- **On resize**: Updates `m_framebuffer_size` and logs the resize event
- **Shutdown**: Destroys ImGui context, pops the input listener, releases RHI resources
- `getContext()` returns the raw `ImGuiContext*` for advanced usage

## Flow
1. `Application::initialize()` → `ui::createImGuiService()` → `ImGuiService::initialize()`:
   - Creates ImGui context, sets error callback
   - Loads ImGui shader module from source string
   - Creates input layout for ImDrawVert (pos/uv/col)
   - Creates font texture from ImGui's rasterized font
   - Creates ImGui render pipeline
   - Registers raw key callback with `IInputService` to route engine key/mouse events into ImGui IO
   - Pushes `ImGuiService`'s `InputListener` onto the input service so ImGui receives events first
2. Per-frame `Application::update()` → `m_ui_service->newFrame(dt)`:
   - Sets ImGui IO display size from framebuffer size
   - Sets DeltaTime from dt
   - Routes keyboard chars, mouse position, wheel, and modifier keys into ImGui IO
   - Calls `ImGui::NewFrame()`
3. Per-frame `Application::render()` → `m_renderer->render(viewports)` includes `ui_entry` with `ui_draw` lambda:
   - Calls `m_ui_service->renderOverlay(pass, fb_size)`
   - ImGui::Render() → passes render state → draws indexed primitives with vertex buffers
4. `Application::shutdown()` → `m_ui_service->shutdown()`:
   - Waits on command queue
   - Clears frame resources
   - Destroys font texture, view, sampler, pipeline, layout, program, queue
   - Pops input listener from input service
   - Destroys ImGui context
   - Nulls all service pointers

## Integration
- **Consumers**: `Application` (creates and uses `IUIService` via `ui::createImGuiService()`), `ImGui` (external library, brought in via `export import imgui`), `IInputService` (routes events to ImGui listener), `IRhiService` (device, queue, surface for swap chain and font texture), `IWindowService` (window framebuffer size)
- **Depends on**: `engine.core`, `engine.math` (int2, float2), `engine.rhi` (IDevice, ICommandQueue, IRenderPipeline, IInputLayout, IShaderProgram, ITexture, ITextureView, ISampler, Format, RenderTargetDesc, ColorTargetDesc, RenderPipelineDesc, InputElementDesc, BufferDesc, ShaderCursor, ResourceState), `engine.shader` (IShaderService, loadModuleFromSource), `std` (memory, format), `imgui` (external, brought in via `export import imgui`)
- **Provides**: `engine.app:ui.imgui` module namespace with `createImGuiService()`, `IUIService` forward declaration, `ImGuiService` class
- **Used by**: `Application::initialize()` creates ImGui service; `Application::update()` calls `m_ui_service->newFrame(dt)`; `Application::render()` calls `m_ui_service->renderOverlay()`; `GlfwPlatform::initialize()` inserts ImGui service into `m_ui_services`

## Key Files
- `App.UI.ImGui.cppm` — `IUIService` forward declaration, `createImGuiService()` declaration, namespace `pP::ui` with `createImGuiService()`
- `App.UI.ImGui.cpp` — `ImGuiService` full implementation (initialize, newFrame, renderOverlay, shutdown, onResize, getContext, createFontTexture_, uploadDrawData_, all RHI resource management, input routing)