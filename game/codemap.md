# game/

## Responsibility

Demo executable (`app.game`): the thin `demo::TurboLarbin : ApplicationEditor` subclass (from the `engine.app` C++23 module) plus `main()` that constructs it (`"ppr"`, argv span via `pP::checked_cast<std::size_t>(argc)` — `int`→`size_t` sign-conversion safe under Clang `-Wconversion -Werror`) and returns `app.run().value()` as the exit code. The editor partition SHIPS — `main.cpp` imports `ApplicationEditor` unconditionally, so do not attempt to gate it behind `BUILD_TESTING`/`PPR_ENABLE_UNIT_TEST`. It hosts the `Application` run loop without owning engine behavior: scene, player, camera, viewport, and UI state are inherited from `ApplicationEditor`/`Application`; game code only adds lifecycle-hook overrides and a debug-only ImGui demo window (timed-shutdown debug hook present but disabled behind `#if 0`).

## Design

- Patterns/abstractions: template-method lifecycle hooks (`initialize()` / `update(const TimeSpan)` / `shutdown()` returning `std::error_code`), inherited constructors (`using super_t::super_t` where `super_t = ApplicationEditor`), service-locator lookup with validity guard (`getServices().get<IUIService>()` + `isValid()`), thin `main` glue delegating to `Application::run()`.
- `main.cpp` — `TurboLarbin` overrides `initialize` (base init via `PPR_RETURN_ERROR_ON_FAIL`; timed-shutdown debug schedule present but disabled behind `#if 0`), `update(const TimeSpan dt)` (forward `dt` to base, then under `#if PPR_ENABLE_DEBUG` resolve `IUIService`, set the ImGui current context from `getContext()`, show `ShowDemoWindow` via a function-local `static bool`), `shutdown()` (base shutdown only). `PPR_DEFINE_LOG_CATEGORY(Demo, …)` + error-code propagation via `PPR_RETURN_ERROR_ON_FAIL`.
- `CMakeLists.txt` — `add_executable(app.game main.cpp)` + `setup_ppr_project(... engine.core/app/math/shader/rhi)`; two POST_BUILD steps: (1) copy `$<TARGET_RUNTIME_DLLS:app.game>` (transitive Windows DLLs, e.g. Slang) next to the exe — replaces any hardcoded DLL list — guarded by `if(WIN32)` (the genex is empty on Linux, which would degrade the copy command into a usage error); (2) unconditional copy `assets/shaders` → `<exe-dir>/shaders`.
- Imports only four engine C++23 modules (`engine.core/math/rhi/app`) + `imgui_internal` + `std` — `engine.shader` arrives transitively via `engine.app`/`engine.rhi`, not via a direct import in `main.cpp`.

## Flow

1. `main(argc, argv)` → `TurboLarbin("ppr", std::span(&argv[0], checked_cast<size_t>(argc)))` → `app.run()` (dirs, platform/services, window, renderer, camera, UI bootstrap owned by `Application`/`ApplicationEditor`).
2. `run()` → `initialize()` chain: `ApplicationEditor::initialize()` first, then the disabled timed-shutdown hook (no-op).
3. Loop per frame until cancel/exit: `ApplicationEditor::update(dt)` → `TurboLarbin::update(dt)` tail (debug-only `IUIService` lookup → `SetCurrentContext` → `ShowDemoWindow`) → engine `render()` of submitted work.
4. Teardown: `TurboLarbin::shutdown()` → `ApplicationEditor::shutdown()`; `main` returns `err.value()`.
5. POST_BUILD assets (Windows-only DLLs + always-copied `shaders/`) must land next to the exe or startup file loads fail.

## Integration

- **Consumers**: run configuration / built `app.game` binary.
- **Depends on**: link-time all five engine libs (core/app/math/shader/rhi) but imports four C++23 modules (`engine.core/math/rhi/app`); `ApplicationEditor` + `Application` run loop + `IUIService` from `engine.app`; `time::now()` / `TimePoint` / `TimeSpan` / `std::error_code` plumbing from `engine.core`; DearImGui module bindings (`imgui_internal`); runtime `shaders/` asset dir + copied DLLs.
- **Provides**: process entry point only — no library, no reusable namespace.

## Key Files

- `main.cpp` — `TurboLarbin` subclass + `main()`.
- `CMakeLists.txt` — `app.game` target, DLL + shader POST_BUILD copies.
