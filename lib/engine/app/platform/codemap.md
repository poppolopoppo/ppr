# lib/engine/app/platform

## Responsibility
The `engine.app:platform` module provides the `IPlatform` interface plus platform error/version infrastructure. `IPlatform` is the central abstraction bridging engine core with OS-specific windowing, input, player, and RHI integration. Backend implementations (currently GLFW in `glfw/`, covering Windows/Linux/macOS) are created via the `IPlatform::create()` factory. This directory holds only the interface partition; all GLFW backend files live in `glfw/` (see `glfw/codemap.md`).

## Design
- **IPlatform** inherits from `safe_object` — instances are factory-owned (`std::unique_ptr<IPlatform>` from `create()`), referenced elsewhere via non-owning `safe_ptr`
- Nine pure virtual methods: `initialize(Application&)`, `shutdown(Application&)`, `getPlatformName()`, `getPlatformVersion()`, `getApplication()`, `getInputService()`, `getWindowService()`, `getPlayerService()`, `update(TimeSpan dt)`
- `create()` factory (defined in the GLFW backend translation unit) returns `std::make_unique<GlfwPlatform>()`; there is no global `IPlatform::get()` singleton
- Platform-specific `errc` enum (`ok`, `fail`, `initialization_failed`, `invalid_argument`) integrates with `std::error_code` via `std::is_error_code_enum` specialization; helpers `platform::error_category()`, `make_error_code(int)`, `make_error_code(errc)`, `result(int)`
- `Version` struct captures major/minor/revision (populated by the backend via `glfwGetVersion()` at query time)
- `SharedPlatform` typedef is `safe_ptr<IPlatform>` (non-owning lifetime-checked view, not shared ownership)

## Flow
1. Startup creates the backend via `IPlatform::create()` and stores it owning-side (e.g. `Application`); `initialize(app)` caches `m_application` and registers created services into the app's `ServicesStore`
2. Per-frame `update(dt)` drives backend polling (GLFW backend polls input devices before window events — see `glfw/codemap.md`)
3. Teardown `shutdown(app)` erases backend services from the store in reverse-dependency order, releases native resources, and resets `m_application` (idempotent second call is a no-op)

## Integration
- **Consumers**: `Application` (primary owner of the `unique_ptr<IPlatform>`), backend implementations in `glfw/` (`GlfwPlatform`, `GlfwWindow`, `GlfwInput`, `GlfwPlayer`)
- **Depends on**: `engine.core` (safe_object, safe_ptr, TimeSpan/TimePoint, error helpers), `std` (string_view, error_code, unique_ptr)
- **Provides**: `engine.app:platform` module namespace with `IPlatform`, `SharedPlatform`, `platform::errc`, `platform::Version`, `platform::error_category/make_error_code/result`
- **Used by**: owning application shell (factory `create()` + `initialize`/`shutdown`/`update`), service lookup `getServices().get<IWindowService/IInputService/IPlayerService>()`, backend `getApplication/getInputService/getWindowService/getPlayerService` accessors; backend details documented in `glfw/codemap.md`

## Key Files
- `App.Platform.cppm` — `IPlatform` declaration, `create()` factory, `errc` enum, `Version` struct, error helpers, `SharedPlatform` typedef
- `glfw/` — GLFW backend (`GlfwPlatform`, `GlfwWindow`, `GlfwInput`, `GlfwPlayer`); documented separately in `glfw/codemap.md`
