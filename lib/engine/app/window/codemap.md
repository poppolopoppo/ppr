# lib/engine/app/window

## Responsibility
The `engine.app:window` module provides the `IWindowService` interface and the GLFW concrete implementation for window creation, lifecycle management, monitor enumeration, and event handling. It abstracts platform-specific window operations (creation, resizing, closing, input, clipboard, monitoring) behind a consistent C++20 interface that integrates with the engine's service locator pattern. The window service is the primary conduit between the OS/GLFW and the engine's application loop.

## Design
- **IWindowService** inherits from `IService` — provides compile-time type-safe lookup via `typeUid<IWindowService>()`. Thirty-two pure virtual methods covering every window operation: creation, destruction, manipulation, focus, monitoring, callbacks, clipboard, and scaling.
- **Window model** (`WindowModel`) — plain data struct with fields: `title`, `window_size`, `framebuffer_size`, `visible`, `focused`, `decorated`, `resizable`. Passed by rvalue reference to `createWindow()`.
- **Window** class (opaque, holds `WindowHandle` = `Numeric<void*, Window>` + `WindowModel`) — the owned window entity. `WindowHandle` is a numeric handle combining a pointer to the `Window` object and the type tag. `SharedWindow` = `safe_ptr<const Window>`.
- **Monitor** class — holds `MonitorHandle` (`Numeric<void*, Monitor>`), `std::string name`, `VideoMode` (resolution, RGB bits, refresh rate), `physical_size`, `virtual_position`, `m_primary_monitor` flag.
- **VideoMode** — `int2 resolution`, `int3 rgb_bits`, `u32 refresh_rate`.
- GLFW backend (`GlfwWindow` inherits `IWindowService`) — singleton `GlfwWindow::get()` manages the single GLFW instance's window list, monitor list, and callback registration. All GLFW callbacks (close, focus, resize, iconify, framebuffer, content-scale, key, char, mouse-button, cursor-position, scroll) are registered in `createWindow()` and operate on the `Window` object's user pointer.
- Callbacks are `Callback<std::error_code(const Window&)>` or similar — consumers register via `whenWindowCreated`, `whenWindowResized`, `whenWindowFocused`, etc.
- `getNativeHandle(window)` → `glfwGetWin32Window()` for RHI surface creation.
- `setWindowMonitor(window, monitor, position, size)` → `glfwSetWindowMonitor()`.
- `pollEvents()` → `glfwPollEvents()`, `waitEvents()` → `glfwWaitEvents()`.

## Flow
1. `Application` constructor → `IPlatform::get()` → `GlfwPlatform` → `GlfwWindow::get()` (creates singleton GLFW window manager)
2. `Application::initialize()` → `m_platform->initialize(*this)` → `GlfwPlatform::initialize()` → creates initial window via `GlfwWindow::createWindow(WindowModel{title="app name", size=1280x720})` → registers callbacks → inserts `IWindowService` and `IInputService` into `app.getServices()`
3. Per-frame `Application::update()` → `m_cached_window_service->pollEvents()` → `glfwPollEvents()` → processes all pending GLFW events (triggering registered callbacks)
4. Window resize: GLFW `framebuffer_size_callback` fires → updates `Window::m_framebuffer_size` → calls `m_when_resized(window, new_size)` → `Application::onWindowResized_()` → `m_renderer->onResize(new_size)` → reconfigures swap chain
5. Window close: GLFW `window_close_callback` fires → `Window::m_visible = false` → `m_when_closed(window)` → `Application::requestApplicationExit()` sets `m_should_close = true`
6. `Application::shutdown()` → `m_platform->shutdown(*this)` → `GlfwWindow::shutdown()` → destroys all GLFW windows, clears monitor callbacks, `glfwTerminate()`

## Integration
- **Consumers**: `Application` (primary — retrieves `m_cached_window_service` via `getServices().get<IWindowService>()`, handles `onWindowResized_`, polls events, checks `getWindowShouldClose`), `RHI` (via `m_renderer->createSurface_()` which calls `window_service.getNativeHandle()`), `ImGuiService` (routes events via input service), `GlfwPlatform` (primary implementer)
- **Depends on**: `engine.core` (IService, typeUid, safe_ptr, hash_t, Numeric), `engine.math` (int2, float2), `std` (string_view, error_code), `engine.app:service.window` (IWindowService interface), `engine.app:platform.glfw` (GlfwWindow implementation)
- **Provides**: `engine.app:window` module namespace with `IWindowService`, `Window`, `WindowModel`, `WindowHandle`, `SharedWindow`, `Monitor`, `VideoMode`, `MonitorHandle`, `SharedMonitor`, `errc` error codes (via `IService` integration)
- **Used by**: `Application` (m_cached_window_service, m_main_window, onWindowResized_, getWorkingDir/ContentDir resolution via platform), `GlfwPlatform::initialize()` (creates initial window, inserts into services), `Renderer::createSurface_()` (gets native handle for RHI surface), `GlfwInput` (routes key/mouse events via GLFW callbacks)

## Key Files
- `App.Window.Handle.cppm` — `Window`, `WindowModel`, `WindowHandle`, `SharedWindow`, `Monitor`, `VideoMode`, `MonitorHandle`, `SharedMonitor` declarations
- `App.Window.Handle.cpp` — (minimal; most definitions are in the .cppm interface file, with implementations in GlfwWindow.cpp)
- `App.Window.Monitor.cppm` — Monitor/VideoMode declarations (may be included in Handle.cppm)
- `App.Window.Monitor.cpp` — Monitor method implementations
- `App.Platform.Glfw.Window.cppm` — `GlfwWindow` class declaration (inherits IWindowService)
- `App.Platform.Glfw.Window.cpp` — GlfwWindow method implementations (createWindow, destroyWindow, pollEvents/waitEvents, all callbacks, manipulation, monitor, clipboard, native handle)
- `App.Window.Monitor.cppm` — Monitor/VideoMode type declarations
- `App.Window.Monitor.cpp` — Monitor method implementations