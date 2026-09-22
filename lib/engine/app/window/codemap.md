# lib/engine/app/window

## Responsibility
The `engine.app:window` module provides the `IWindowService` interface plus the `Window`/`Monitor` models and `Viewport` geometry for window creation, lifecycle management, monitor enumeration, and event handling. It abstracts platform-specific window operations (creation, resizing, closing, input, clipboard, monitoring) behind a consistent C++20 interface that integrates with the engine's service locator pattern. The GLFW concrete implementation (`GlfwWindow`) lives in `engine.app:platform.glfw` (see `platform/glfw/codemap.md`), not here.

## Design
- **IWindowService** inherits from `IService` — provides compile-time type-safe lookup via `typeUid<IWindowService>()`. Thirty-two pure virtual methods covering every window operation: creation, destruction, manipulation, focus, monitoring, callbacks, clipboard, and scaling.
- **Window model** (`WindowModel`) — plain data struct: `m_title`, `m_fullscreen_monitor`, `m_share_resources_with`, `m_window_position`, `m_window_size`, bitfield flags `m_decorated`/`m_focused`/`m_iconified`/`m_resizable`/`m_visible`. No framebuffer size here — that lives on `Window`.
- **Window** class (`WindowModel` + `safe_object`, move-only, copy deleted, move-assign deleted) — the owned window entity. Holds opaque `WindowHandle` (`Numeric<void*, Window>`) + `NativeWindowHandle` (`Numeric<void*, WindowHandle>`), `m_content_scale`, `m_framebuffer_size`, `m_hovered`, and `WindowDelegate` event fields: `m_when_closed`, `m_when_focused/hovered/iconified`, `m_when_moved/resized/scaled`, `m_when_character_input`, `m_when_keyboard_pressed/repeated`, `m_when_mouse_clicked/moved/scrolled`, `m_when_drag_and_dropped`. Ctor asserts non-null handles; move ctor transfers `m_handle`; `release()` swaps the handle out to null; debug dtor asserts the handle was released. `SharedWindow` = `safe_ptr<const Window>`. Linux/clang bring-up: the seven `extern template function_ref<...Window...>` declarations were removed from `App.Window.Handle.cppm` — explicit instantiation now lives centrally in `App.TemplateInstantiations.cpp`, so the header no longer pins `function_ref` specializations per TU.
- **Monitor** class — holds `MonitorHandle` (`Numeric<void*, Monitor>`), `std::string name`, `VideoMode` (resolution, RGB bits, refresh rate), `physical_size`, `virtual_position`, `m_primary_monitor` flag.
- **VideoMode** — `int2 resolution`, `int3 rgb_bits`, `u32 refresh_rate`.
- GLFW backend (`GlfwWindow` inherits `IWindowService`) — singleton `GlfwWindow::get()` manages the single GLFW instance's window list, monitor list, and callback registration. All GLFW callbacks (close, focus, resize, iconify, framebuffer, content-scale, key, char, mouse-button, cursor-position, scroll) are registered in `createWindow()` and operate on the `Window` object's user pointer.
- Callbacks are `Callback<std::error_code(const Window&)>` or similar — consumers register via `whenWindowCreated`, `whenWindowResized`, `whenWindowFocused`, etc.
- `getNativeHandle(window)` → `glfwGetWin32Window()` for RHI surface creation.
- `setWindowMonitor(window, monitor, position, size)` → `glfwSetWindowMonitor()`.
- `pollEvents()` → `glfwPollEvents()`, `waitEvents()` → `glfwWaitEvents()`.
- **Viewport geometry** (`engine.app:window.viewport`, moved here from the deleted renderer `App.Viewport.cpp/.cppm`):
  `BasicRect<T>` (origin/extent rect with `fromAabb`/`fromSize`, edge getters/setters, `getAspectRatio` asserting
  non-zero height, `contains` point/rect, `normalize`/`denormalize` with `Clamp` variants on a pixel-center
  convention), `PixelRect` (screen pixels) / `NormalizedRect` (window-space UV), `ViewportLayout` policy variant
  (`FullWindow`/`Centered`/`WindowRect`/`NormalizedWindowRect` with `clientRect(window_rect)` — normalized maps with a
  `+0.5f` origin bias and `round`), immutable `Viewport` (window + client rects, screen/window/client transforms,
  client/window normalize helpers, degenerate-window guard collapsing the client to zero extent), and `WindowViewport`
  (fixed `SharedWindow` binding + `ViewportRevision` bumped only on change, `setLayout` re-derives, owner-driven
  `updateFromWindow()` after event polling — holds no window subscription). Linux/clang bring-up: `normalizeClamp` /
  `denormalizeClamp` pass explicit dims (`saturate<float, 2u>`) so mango `int`-vs-`u32` deduction matches the `float2` boundary.

## Flow
1. `Application` constructor → `IPlatform::get()` → `GlfwPlatform` → `GlfwWindow::get()` (creates singleton GLFW window manager)
2. `Application::initialize()` → `m_platform->initialize(*this)` → `GlfwPlatform::initialize()` → creates initial window via `GlfwWindow::createWindow(WindowModel{title="app name", size=1280x720})` → registers callbacks → inserts `IWindowService` and `IInputService` into `app.getServices()`
3. Per-frame `Application::update()` → `m_cached_window_service->pollEvents()` → `glfwPollEvents()` → processes all pending GLFW events (triggering registered callbacks)
4. Window resize: GLFW framebuffer callback fires → updates `Window::m_framebuffer_size` → `m_when_resized(window, old_size)` → `Application::onWindowResized_()` → `m_renderer.resizeWindowSurface(handle, framebuffer_size)` (+ `m_ui_service->onResize`) → reconfigures surface
5. Window close: GLFW `window_close_callback` fires → `Window::m_visible = false` → `m_when_closed(window)` → `Application::requestApplicationExit()` sets `m_should_close = true`
6. `Application::shutdown()` → `m_platform->shutdown(*this)` → `GlfwWindow::shutdown()` → destroys all GLFW windows, clears monitor callbacks, `glfwTerminate()`
7. Viewport clients: construct `Viewport(window, layout)` or `WindowViewport(window, layout)` → after each `pollEvents()` call `updateFromWindow()` → pass the resulting `Viewport` to `Camera::updateModel` and `makeRenderView(viewport, target_extent)` for submission scissoring

## Integration
- **Consumers**: `Application` (primary — retrieves `m_cached_window_service` via `getServices().get<IWindowService>()`, handles `onWindowResized_`, polls events, checks `getWindowShouldClose`), `RHI` (via `m_renderer.createWindowSurface()` which calls `window_service.getNativeHandle()`), `ImGuiService` (routes events via input service), `GlfwPlatform` (primary implementer), `engine.app:renderer.types` (`makeRenderView` consumes `Viewport`), `engine.app:scene.camera` (`Camera::updateModel` consumes `Viewport`)
- **Depends on**: `engine.core` (IService, typeUid, safe_ptr, hash_t, Numeric), `engine.math` (int2, float2), `std` (string_view, error_code), `engine.app:service.window` (IWindowService interface), `engine.app:platform.glfw` (GlfwWindow implementation)
- **Provides**: `engine.app:window` module namespace with `IWindowService`, `Window`, `WindowModel`, `WindowHandle`, `NativeWindowHandle`, `SharedWindow`, `Monitor`, `VideoMode`, `MonitorHandle`, `SharedMonitor`, `errc` error codes (via `IService` integration), plus `engine.app:window.viewport` (`BasicRect`/`PixelRect`/`NormalizedRect`, `ViewportLayout`, `Viewport`, `WindowViewport` — moved here from the deleted renderer `App.Viewport.cpp/.cppm`)
- **Used by**: `Application` (m_cached_window_service, m_main_window, onWindowResized_, getWorkingDir/ContentDir resolution via platform), `GlfwPlatform::initialize()` (creates initial window, inserts into services), `Renderer::createWindowSurface()` (gets native handle for RHI surface), `GlfwInput` (routes key/mouse events via GLFW callbacks)

## Key Files
- `App.Window.Handle.cppm` — `Window`, `WindowModel`, `WindowHandle`, `NativeWindowHandle`, `SharedWindow`, `Monitor`, `VideoMode`, `MonitorHandle`, `SharedMonitor`, `WindowDelegate` declarations
- `App.Window.Handle.cpp` — `Window` move ctor, `release()`, debug-dtor handle check (most behavior lives in the .cppm + `GlfwWindow` in `platform/glfw/`)
- `App.Window.Monitor.cppm` — Monitor/VideoMode declarations
- `App.Window.Monitor.cpp` — Monitor method implementations
- `App.Window.Viewport.cppm` — `BasicRect`/`PixelRect`/`NormalizedRect`, `ViewportLayout`, `Viewport`, `WindowViewport` declarations (moved from renderer `App.Viewport`)
- `App.Window.Viewport.cpp` — `BasicRect` aspect ratio, `ViewportLayout::clientRect`, `Viewport` transforms, `WindowViewport::updateFromWindow` implementations
