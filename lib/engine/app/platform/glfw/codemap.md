# lib/engine/app/platform/glfw

## Responsibility
The `engine.app:platform.glfw` module provides the GLFW implementation of the `IPlatform` interface, as well as the `GlfwInput` (implements `IInputService`), `GlfwWindow` (implements `IWindowService`), and `GlfwPlayer` (implements `IPlayerService`). This is the concrete platform backend that GLFW-based applications use to integrate with the engine. It handles window creation, input routing, monitor enumeration, gamepad hot-plugging, and player lifecycle management.

## Design
- **GlfwPlatform** implements `IPlatform` — owns `GlfwWindow` and delegates to it for window management; creates and owns `GlfwInput` for input; exposes `IInputService`, `IWindowService`, `IPlayerService` via `getInputService()`, `getWindowService()`, `getPlayerService()`
- **GlfwWindow** implements `IWindowService` — singleton (`GlfwWindow::get()`) manages GLFW window lifecycle, monitor enumeration via `glfwGetMonitors/glfwGetVideoMode`, callback registration (close, focus, resize, iconify, framebuffer, content-scale, key, char, mouse-button, cursor-position, scroll), and native handle extraction (`glfwGetWin32Window`)
- **GlfwInput** implements `IInputService` — singleton (`GlfwInput::get()`) holds hot per-frame device state (`KeyboardDevice`, `MouseDevice`, `std::array<GamepadDevice, 4>`), routes messages through player graph, dispatches to global/pushed listeners, supports recording/playback mode via `InputReplay` decorator
- **GlfwPlayer** implements `IPlayerService` — singleton (`GlfwPlayer::get()`) created with reference to `GlfwInput`; maps devices to players via `PlayerGraph`; lazily creates keyboard player on first access; handles gamepad hot-plug/unplug and player removal
- All three singletons use static local instance pattern (Meyers' singleton) thread-safe in C++11+
- `GlfwPlatform::initialize()` creates `GlfwWindow`, `GlfwInput`, `GlfwPlayer`, and inserts all three into the application's `ServicesStore`
- `GlfwPlatform::shutdown()` shuts down window/input/player, erases services, and calls `glfwTerminate()`

## Flow
1. `Application` constructor → `IPlatform::get()` → returns `&g_glfw_platform` static instance
2. `Application::initialize()` → `m_platform->initialize(*this)` → GlfwPlatform initializes GLFW, creates `GlfwWindow::get()` (enumerates monitors), creates `GlfwInput::get()`, creates `GlfwPlayer::get()` (ties to GlfwInput), inserts all into `app.getServices()`
3. Per-frame: `Application::update()` → `m_cached_window_service->pollEvents()` → `glfwPollEvents()` → GLFW callbacks fire → `GlfwInput::onKey/onMouseButton/onCursorPos/onScroll` → update hot state → `postInputMessages(dt)` → routes through player graph → delivers to `InputListener`s
4. Gamepad hot-plug: `GlfwInput::pollGamepads_()` detects `glfwJoystickPresent()` changes → connects/disconnects gamepad → creates/removes player via `GlfwPlayer`
5. `Application::render()` → `m_renderer->render(viewports)` → submits multi-viewport frame (scene + UI)
6. `Application::shutdown()` → `m_platform->shutdown(*this)` → GlfwPlatform shuts down window/input/player, terminates GLFW

## Integration
- **Consumers**: `Application` (primary), `GlfwInput`, `GlfwWindow`, `GlfwPlayer`, `ImGuiService` (pushes listener to GlfwInput)
- **Depends on**: `engine.core` (safe_object, safe_ptr, IService), `engine.math` (int2, float2), `std` (string_view, error_code, monostate), `engine.rhi`, `engine.shader`
- **Provides**: `engine.app:platform.glfw` module namespace with `GlfwPlatform`, `GlfwWindow`, `GlfwInput`, `GlfwPlayer`
- **Used by**: `Application` (m_platform member, services store), `IPlatform::get()`, `GlfwPlatform::initialize()` inserts into services, `GlfwInput::postInputMessages()` drives per-frame input

## Key Files
- `App.Platform.Glfw.cppm` — `GlfwPlatform` class declaration (inherits IPlatform)
- `App.Platform.Glfw.cpp` — GlfwPlatform method implementations (initialize, shutdown, getInputService/getWindowService/getPlayerService, platform name/version, error category)
- `App.Platform.Glfw.Window.cppm` — `GlfwWindow` class declaration (inherits IWindowService)
- `App.Platform.Glfw.Window.cpp` — GlfwWindow method implementations (initialize, shutdown, createWindow, destroyWindow, monitor enumeration, pollEvents/waitEvents, all callbacks, manipulation, clipboard)
- `App.Platform.Glfw.Input.cppm` — `GlfwInput` class declaration (inherits IInputService)
- `App.Platform.Glfw.Input.cpp` — GlfwInput method implementations (onKey/onChar/onMouseButton/onCursorPos/onScroll, glfwToKeyboardKey_, pollGamepads_, feedGamepad_, dispatchToGlobal/PushedListeners, routeMessage_, postInputMessages, resetInputState, listener/mapping callbacks)
- `App.Platform.Glfw.Player.cppm` — `GlfwPlayer` class declaration (inherits IPlayerService)
- `App.Platform.Glfw.Player.cpp` — GlfwPlayer method implementations (getOrCreateKeyboardPlayer, addGamepadPlayer, removePlayer, whenPlayerAdded/Removed, resetPlayers, static get(), IPlayerService::get())