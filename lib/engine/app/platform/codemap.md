# lib/engine/app/platform

## Responsibility
The `engine.app:platform` module provides the `IPlatform` interface and platform error/version infrastructure. `IPlatform` is the central abstraction that bridges the engine core with OS-specific windowing, input, player, and RHI integration. It is implemented by platform backends (currently GLFW in `glfw/`, covering Windows/Linux/macOS) and registered as a singleton service via the engine's service locator. This directory now holds only the interface partition; all GLFW backend files live in `glfw/` (see `glfw/codemap.md`).

## Design
- **IPlatform** inherits from `IService` — provides compile-time type-safe lookup via `typeUid<IPlatform>()` / `ServicesStore`
- **Static `IPlatform::get()`** returns a `SharedPlatform` (raw pointer in debug, `safe_ptr` in release) to a globally unique instance
- Seven pure virtual methods: `initialize(Application&)`, `shutdown(Application&)`, `getPlatformName()`, `getPlatformVersion()`, `getInputService()`, `getWindowService()`, `getPlayerService()`
- Platform-specific `errc` enum (`ok`, `fail`, `initialization_failed`, `invalid_argument`) integrates with `std::error_code`
- `Version` struct captures major/minor/revision via `glfwGetVersion()` at init time
- `IPlatform` is stored in `Application.m_platform` (`safe_ptr<IPlatform>`) and retrieved via `m_services.get<IPlatform>()` or `IPlatform::get()`

## Flow
1. `Application::Application()` retrieves `IPlatform::get()` — the singleton GLFW (or other) instance
2. `Application::initialize()` calls `m_platform->initialize(*this)` which initializes the backend, creates the window/input/player services, and inserts them into the app's `ServicesStore`
3. `Application::run()` loop: per-frame `update()` polls events via the cached window service, input poll routes through the `InputContext` tree, `render()` submits viewports via `m_renderer`
4. `Application::shutdown()` calls `m_platform->shutdown(*this)` which tears down backend services, erases them from the store, and releases native resources

## Integration
- **Consumers**: `Application` (primary), backend implementations in `glfw/` (`GlfwPlatform`, `GlfwWindow`, `GlfwInput`, `GlfwPlayer`)
- **Depends on**: `engine.core` (safe_object, safe_ptr, IService), `engine.math` (int2, float2), `std` (string_view, error_code)
- **Provides**: `engine.app:platform` module namespace with `IPlatform`, `errc`, `Version`
- **Used by**: `Application` (m_platform member, getServices().get<IPlatform>()), `IPlatform::get()` static accessor, platform backends in `glfw/`; backend details documented in `glfw/codemap.md`

## Key Files
- `App.Platform.cppm` — `IPlatform` class declaration, `errc` enum, `Version` struct, `SharedPlatform` typedef
- `glfw/` — GLFW backend (`GlfwPlatform`, `GlfwWindow`, `GlfwInput`, `GlfwPlayer`); documented separately in `glfw/codemap.md`
