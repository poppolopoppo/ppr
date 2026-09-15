# lib/engine/app/platform

## Responsibility
The `engine.app:platform` module is the HAL-facing platform abstraction for `engine.app`. It defines the `IPlatform` interface plus platform error/version infrastructure, isolating OS-specific windowing, input, player, and event-pump mechanics from `Application` and `Renderer`. Backend implementations live below this directory (currently GLFW in `glfw/`, covering Windows/Linux/macOS) and are selected via the `IPlatform::create()` factory. This directory holds only the interface partition; all `GlfwPlatform`/`GlfwWindow`/`GlfwInput`/`GlfwPlayer` behavior is documented in `glfw/codemap.md` and summarized here.

## Design
- **IPlatform** inherits from `safe_object` — instances are factory-owned (`std::unique_ptr<IPlatform>` from `create()`), referenced elsewhere via non-owning `safe_ptr` (`SharedPlatform` typedef)
- Nine pure virtual methods: `initialize(Application&)`, `shutdown(Application&)`, `getPlatformName()`, `getPlatformVersion()`, `getApplication()`, `getInputService()`, `getWindowService()`, `getPlayerService()`, `update(TimeSpan dt)`; there is no global `IPlatform::get()` singleton
- HAL isolation contract: `initialize` caches the owning `Application` and registers created services into the app's `ServicesStore`; `update(dt)` drives the backend event pump; `shutdown(app)` erases services in reverse-dependency order and releases native resources (idempotent)
- Domain-gated service creation (enforced by the backend): headless suppresses window creation, non-interactive suppresses input, no-presence suppresses player — see `glfw/codemap.md` for the `GlfwWindow`/`GlfwInput`/`GlfwPlayer` gating
- Platform-specific `errc` enum (`ok`, `fail`, `initialization_failed`, `invalid_argument`) integrates with `std::error_code` via `std::is_error_code_enum` specialization; helpers `platform::error_category()`, `make_error_code(int)`, `make_error_code(errc)`, `result(int)`
- `Version` struct captures major/minor/revision (populated by the backend via `glfwGetVersion()` at query time)
- GLFW backend summary (detail in `glfw/codemap.md`): `GlfwPlatform` owns `unique_ptr<GlfwWindow/GlfwInput/GlfwPlayer>` plus an application view; installs process-lifetime GLFW allocator hooks and error-callback routing; `GlfwWindow` owns monitor/window lifecycle with dual-level (window + service) callbacks, Win32 native handles, and clipboard; `GlfwInput` owns context-routed keyboard/mouse/gamepad polling with joystick hot-plug; `GlfwPlayer` owns graph-backed keyboard/gamepad player lifecycle over the input devices

## Flow
1. Startup `IPlatform::create()` returns the backend (`std::make_unique<GlfwPlatform>()`); the owner (e.g. `Application`) stores it and calls `initialize(app)` → backend installs error callback + allocator, `glfwInit()`, then domain-gated `GlfwWindow::initialize()` (monitor enumeration) → `GlfwInput::initialize()` (keyboard/mouse registration, joystick callback) → `GlfwPlayer::initialize(input)` → each service inserted into `app.getServices()`
2. Per-frame `update(dt)` drives backend polling in input-first order: `GlfwInput::pollInputDevices(dt)` (timestamp advance, transient-state flush, per-pad `glfwGetGamepadState` → context dispatch) then `GlfwWindow::pollEvents()` (`glfwPollEvents()` → static C-callbacks → window-level + service-level sinks feeding `GlfwInput::post*`)
3. Presentation/events outward: window service owns `Window` handles consumed by `Renderer::renderAndPresent(window, …)`; gamepad hot-plug callbacks register/erase devices and drive player graph add/remove
4. Teardown `shutdown(app)` erases backend services from the store in reverse-dependency order (player → input → window, retain-first-error), resets each service, then `glfwTerminate()` + clears the error callback and resets the application view (idempotent second call is a no-op)

## Integration
- **Consumers**: `Application` (primary owner of the `unique_ptr<IPlatform>`), backend implementations in `glfw/` (`GlfwPlatform`, `GlfwWindow`, `GlfwInput`, `GlfwPlayer`)
- **Depends on**: `engine.core` (safe_object, safe_ptr, TimeSpan/TimePoint, error helpers), `std` (string_view, error_code, unique_ptr)
- **Provides**: `engine.app:platform` module namespace with `IPlatform`, `SharedPlatform`, `platform::errc`, `platform::Version`, `platform::error_category/make_error_code/result`
- **Used by**: owning application shell (factory `create()` + `initialize`/`shutdown`/`update`), service lookup `getServices().get<IWindowService/IInputService/IPlayerService>()`, backend `getApplication/getInputService/getWindowService/getPlayerService` accessors; backend details documented in `glfw/codemap.md`

## Key Files
- `App.Platform.cppm` — `IPlatform` declaration, `create()` factory, `errc` enum, `Version` struct, error helpers, `SharedPlatform` typedef
- `glfw/` — GLFW backend (`GlfwPlatform`, `GlfwWindow`, `GlfwInput`, `GlfwPlayer`); documented separately in `glfw/codemap.md`
