# lib/engine/app/platform

## Responsibility
The `engine.app:platform` module provides the `IPlatform` interface and platform-specific error infrastructure. `IPlatform` is the central abstraction that bridges the engine core with the OS-specific windowing, input, player, and RHI integration. It is implemented by platform backends (currently GLFW on Windows/Linux/macOS) and registered as a singleton service via the engine's service locator. The module also defines `errc` error codes and `Version` metadata for diagnostics.

## Design
- **IPlatform** inherits from `IService` — provides compile-time type-safe lookup via `typeUid<IPlatform>()` / `ServicesStore`
- **Static `IPlatform::get()`** returns a `SharedPlatform` (raw pointer in debug, `safe_ptr` in release) to a globally unique instance
- Seven pure virtual methods: `initialize(Application&)`, `shutdown(Application&)`, `getPlatformName()`, `getPlatformVersion()`, `getInputService()`, `getWindowService()`, `getPlayerService()`
- Platform-specific `errc` enum (`ok`, `fail`, `initialization_failed`, `invalid_argument`) integrates with `std::error_code`
- `Version` struct captures major/minor/revision via `glfwGetVersion()` at init time
- `IPlatform` is stored in `Application.m_platform` (`safe_ptr<IPlatform>`) and retrieved via `m_services.get<IPlatform>()` or `IPlatform::get()`

## Flow
1. `Application::Application()` retrieves `IPlatform::get()` — the singleton GLFW (or other) instance
2. `Application::initialize()` calls `m_platform->initialize(*this)` which initializes GLFW, creates the window service, input service, and player service, and inserts them into the app's `ServicesStore`
3. `Application::run()` loop: per-frame `update()` polls events via `m_cached_window_service->pollEvents()`, `render()` submits viewports via `m_renderer`
4. `Application::shutdown()` calls `m_platform->shutdown(*this)` which terminates GLFW, erases all platform-related services from the store, and releases resources

## Integration
- **Consumers**: `Application` (primary), `GlfwPlatform`, `GlfwWindow`, `GlfwInput`, `GlfwPlayer`
- **Depends on**: `engine.core` (safe_object, safe_ptr, IService), `engine.math` (int2, float2), `std` (string_view, error_code)
- **Provides**: `engine.app:platform` module namespace with `IPlatform`, `errc`, `Version`
- **Used by**: `Application` (m_platform member, getServices().get<IPlatform>()), `IPlatform::get()` static accessor, platform-specific backends

## Key Files
- `App.Platform.cppm` — `IPlatform` class declaration, `errc` enum, `Version` struct, `SharedPlatform` typedef
- `App.Platform.Glfw.cppm` — `GlfwPlatform` class declaration (inherits IPlatform)
- `App.Platform.Glfw.cpp` — GlfwPlatform method implementations (initialize, shutdown, getInputService/getWindowService/getPlayerService, error category, version)
- `App.Platform.Glfw.Window.cppm` — `GlfwWindow` class declaration (inherits IWindowService)
- `App.Platform.Glfw.Window.cpp` — GlfwWindow method implementations (createWindow, pollEvents, monitor callbacks, window callbacks, clipboard, manipulation)
- `App.Platform.Glfw.Input.cppm` — `GlfwInput` class declaration (inherits IInputService)
- `App.Platform.Glfw.Input.cpp` — GlfwInput method implementations (onKey/onMouseButton/onCursorPos/onScroll, pollGamepads_, feedGamepad_, dispatchToListeners, postInputMessages)
- `App.Platform.Glfw.Player.cppm` — `GlfwPlayer` class declaration (inherits IPlayerService)
- `App.Platform.Glfw.Player.cpp` — GlfwPlayer method implementations (getOrCreateKeyboardPlayer, addGamepadPlayer, removePlayer, whenPlayerAdded/Removed, resetPlayers, static get())