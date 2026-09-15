# game/

## Responsibility

Demo executable (`app.game`): the thin `demo::TurboLarbin : ApplicationEditor` subclass (from the `engine.app` C++23 module) plus `main()` that constructs it (`"ppr"`, argv span) and returns `app.run().value()` as the exit code. It hosts the `Application` run loop without owning engine behavior: scene, player, camera, viewport, and UI state are inherited from `ApplicationEditor`/`Application`; game code only adds lifecycle-hook overrides, a startup timestamp, and a debug-only ImGui demo window.

## Design

- Patterns/abstractions: template-method lifecycle hooks (`initialize()` / `update(const TimeSpan)` / `shutdown()` returning `std::error_code`), inherited constructors (`using super_t::super_t` where `super_t = ApplicationEditor`), RAII `std::optional<TimePoint>` startup timer, service-locator lookup with validity guard (`getServices().get<IUIService>()` + `isValid()`), thin `main` glue delegating to `Application::run()`.
- `main.cpp` — `TurboLarbin` overrides `initialize` (base init via `PPR_RETURN_ERROR_ON_FAIL`, then record `time::now()` in `m_started_at`), `update(const TimeSpan dt)` (forward `dt` to base, then under `#if PPR_ENABLE_DEBUG` resolve `IUIService`, set the ImGui current context from `getContext()`, show `ShowDemoWindow` via a function-local `static bool`), `shutdown()` (reset timer, then base shutdown). `PPR_DEFINE_LOG_CATEGORY(Demo, …)` + error-code propagation via `PPR_RETURN_ERROR_ON_FAIL`.
- `CMakeLists.txt` — `add_executable(app.game main.cpp)` + `setup_ppr_project(... engine.core/app/math/shader/rhi)`; two POST_BUILD steps: (1) copy `$<TARGET_RUNTIME_DLLS:app.game>` (transitive Windows DLLs, e.g. Slang) next to the exe — replaces any hardcoded DLL list; (2) copy `assets/shaders` → `<exe-dir>/shaders`.
- Imports only four engine C++23 modules (`engine.core/math/rhi/app`) + `imgui_internal` + `std` — `engine.shader` arrives transitively via `engine.app`/`engine.rhi`, not via a direct import in `main.cpp`.

## Flow

1. `main(argc, argv)` → `TurboLarbin("ppr", std::span(&argv[0], argc))` → `app.run()` (dirs, platform/services, window, renderer, camera, UI bootstrap owned by `Application`/`ApplicationEditor`).
2. `run()` → `initialize()` chain: `ApplicationEditor::initialize()` first, then record `m_started_at = time::now()`.
3. Loop per frame until cancel/exit: `ApplicationEditor::update(dt)` → `TurboLarbin::update(dt)` tail (debug-only `IUIService` lookup → `SetCurrentContext` → `ShowDemoWindow`) → engine `render()` of submitted work.
4. Teardown: `TurboLarbin::shutdown()` resets `m_started_at`, then `ApplicationEditor::shutdown()`; `main` returns `err.value()`.
5. POST_BUILD assets (DLLs + `shaders/`) must land next to the exe or startup file loads fail.

## Integration

- **Consumers**: run configuration / built `app.game` binary.
- **Depends on**: link-time all five engine libs (core/app/math/shader/rhi) but imports four C++23 modules (`engine.core/math/rhi/app`); `ApplicationEditor` + `Application` run loop + `IUIService` from `engine.app`; `time::now()` / `TimePoint` / `TimeSpan` / `std::error_code` plumbing from `engine.core`; DearImGui module bindings (`imgui_internal`); runtime `shaders/` asset dir + copied DLLs.
- **Provides**: process entry point only — no library, no reusable namespace.

## Key Files

- `main.cpp` — `TurboLarbin` subclass + `main()`.
- `CMakeLists.txt` — `app.game` target, DLL + shader POST_BUILD copies.
