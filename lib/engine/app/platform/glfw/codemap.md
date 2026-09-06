# lib/engine/app/platform/glfw

## Responsibility
The `engine.app:platform.glfw` module provides the GLFW implementation of the `IPlatform` interface, as well as `GlfwInput` (implements `IInputService`), `GlfwWindow` (implements `IWindowService`), and `GlfwPlayer` (implements `IPlayerService`). This is the concrete platform backend that GLFW-based applications use to integrate with the engine. It handles window creation, context-routed input polling, monitor enumeration, gamepad hot-plugging, and player lifecycle management.

## Design
- **GlfwPlatform** implements `IPlatform` — owns `GlfwWindow` and delegates to it for window management; creates and owns `GlfwInput` for input; exposes `IInputService`, `IWindowService`, `IPlayerService` via `getInputService()`, `getWindowService()`, `getPlayerService()`
- **GlfwWindow** implements `IWindowService` — singleton (`GlfwWindow::get()`) manages GLFW window lifecycle, monitor enumeration via `glfwGetMonitors/glfwGetVideoMode`, callback registration (close, focus, resize, iconify, framebuffer, content-scale, key, char, mouse-button, cursor-position, scroll), and native handle extraction (`glfwGetWin32Window`)
- **GlfwInput** implements `IInputService` — singleton (`GlfwInput::get()`) holding hot per-frame device state directly: one `KeyboardDevice` (`"Keyboard"`), one `MouseDevice` (`"MouseJoy"`), and `std::array<GamepadDevice, 4>` (`"GamePad0"`–`"GamePad3"`); device classes come from the unified `engine.app:input.device` partition (no `device/` sub-partition remains)
- Context-routed dispatch: `m_global_context` plus `m_all_contexts` registry, `m_devices_by_id` device lookup, and `m_context_by_device` per-device context assignment (`assignInputContextToDevice`/`clearInputContextDeviceAssignments`/`enumerateInputContextDeviceAssignments`); per-window wiring is done by `WindowInputContext`
- `pollInputDevices(dt)` polls keyboard/mouse/gamepad state (including `pollInputGamepad_` per pad) and routes through the assigned contexts; `resetInputDevices()` clears transient state
- Posting entry points (`postKeyboardCharacterInput`, `postKeyboardKeyPressed`, `postMouseButtonPressed`, `postMouseCursorPosition`, `postMouseScrollWheel`) all take the target `InputContext`
- Cold device lifecycle callbacks: `whenDeviceConnected`/`whenDeviceDisconnected` (`DeviceCallback`); `m_gamepads_ever_connected` tracks first-connection state; `m_timestamp`/`m_delta_time` carry per-frame timing
- **GlfwPlayer** implements `IPlayerService` — singleton (`GlfwPlayer::get()`) created with reference to `GlfwInput`; maps devices to players via `PlayerGraph`; lazily creates the unified keyboard(+mouse) player on first access; handles gamepad hot-plug/unplug and player removal
- All three singletons use static local instance pattern (Meyers' singleton) thread-safe in C++11+
- `GlfwPlatform::initialize()` creates `GlfwWindow`, `GlfwInput`, `GlfwPlayer`, and inserts all three into the application's `ServicesStore`
- `GlfwPlatform::shutdown()` shuts down window/input/player, erases services, and calls `glfwTerminate()`

## Flow
1. `Application` constructor → `IPlatform::get()` → returns the static GLFW platform instance
2. `Application::initialize()` → `m_platform->initialize(*this)` → GlfwPlatform initializes GLFW, creates `GlfwWindow::get()` (enumerates monitors), creates `GlfwInput::get()` (registers devices, initializes timing), creates `GlfwPlayer::get()` (ties to GlfwInput), inserts all into `app.getServices()`
3. Per-frame: `Application::update()` → cached window service `pollEvents()` → `glfwPollEvents()` → GLFW callbacks fire → `WindowInputContext` bridges (key/char/mouse-button/mouse-moved/scroll) into `IInputService::post*` with the window's context → `pollInputDevices(dt)` flushes accumulated device state through assigned contexts → `InputListener`s resolve actions
4. Gamepad hot-plug: gamepad poll detects `glfwJoystickPresent()` changes → device connect/disconnect callbacks fire → creates/removes player via `GlfwPlayer`/`PlayerGraph`
5. `Application::render()` → `m_renderer.renderAndPresent(handle, submissions)` → submits scene + UI DrawSubmissions and presents
6. `Application::shutdown()` → `m_platform->shutdown(*this)` → GlfwPlatform shuts down window/input/player, terminates GLFW

## Integration
- **Consumers**: `Application` (primary), `GlfwInput`, `GlfwWindow`, `GlfwPlayer`, `ImGuiService` (registers contexts/listeners with GlfwInput)
- **Depends on**: `engine.core` (safe_object, safe_ptr, IService), `engine.math` (int2, float2), `std` (string_view, error_code, monostate), `engine.app:input.device` (unified devices), `engine.app:input.listener` (`InputContext`), `engine.app:player.graph` (`PlayerGraph`), `engine.rhi`, `engine.shader`
- **Provides**: `engine.app:platform.glfw` module namespace with `GlfwPlatform`, `GlfwWindow`, `GlfwInput`, `GlfwPlayer`
- **Used by**: `Application` (m_platform member, services store), `IPlatform::get()`, `GlfwPlatform::initialize()` inserts into services, `GlfwInput::pollInputDevices()` drives per-frame input

## Key Files
- `App.Platform.Glfw.cppm` — `GlfwPlatform` class declaration (inherits IPlatform)
- `App.Platform.Glfw.cpp` — GlfwPlatform method implementations (initialize, shutdown, getInputService/getWindowService/getPlayerService, platform name/version, error category)
- `App.Platform.Glfw.Window.cppm` — `GlfwWindow` class declaration (inherits IWindowService)
- `App.Platform.Glfw.Window.cpp` — GlfwWindow method implementations (initialize, shutdown, createWindow, destroyWindow, monitor enumeration, pollEvents/waitEvents, all callbacks, manipulation, clipboard)
- `App.Platform.Glfw.Input.cppm` — `GlfwInput` class declaration (inherits IInputService; hot devices, global/per-device contexts, timing, device callbacks)
- `App.Platform.Glfw.Input.cpp` — GlfwInput method implementations (initialize/shutdown, device/context enumeration, assignInputContextToDevice, pollInputDevices/pollInputGamepad_, post* entry points, device connect/disconnect callbacks)
- `App.Platform.Glfw.Player.cppm` — `GlfwPlayer` class declaration (inherits IPlayerService)
- `App.Platform.Glfw.Player.cpp` — GlfwPlayer method implementations (getOrCreateKeyboardPlayer, addGamepadPlayer, removePlayer, whenPlayerAdded/Removed, resetPlayers, static get(), IPlayerService::get())
- `App.Platform.Glfw.include.hpp` — GLFW native-include helper header
