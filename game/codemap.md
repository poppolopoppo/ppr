# game/

## Responsibility

Demo executable (`app.game`): the thin `TurboLarbin : pP::Application` subclass plus `main()` that constructs it
(`ApplicationDomain{}`, `"ppr"`, argv span) and returns `app.run().value()` as the exit code. All engine behavior
lives in `engine.app`; game code only adds lifecycle-hook overrides and a debug-only ImGui demo window.

## Design

- `main.cpp` — `TurboLarbin` overrides `initialize` (base init + record `time::now()` in
  `optional<TimePoint>`), `update(const TimeSpan dt)` (forwards `dt` to base, then in
  `PPR_ENABLE_DEBUG` resolves `IUIService` via `getServices().get<IUIService>()`, guards `isValid()`,
  sets the ImGui current context from `getContext()`, shows `ShowDemoWindow`), `shutdown()` (reset timer,
  then base shutdown). `PPR_DEFINE_LOG_CATEGORY(Demo, …)` + error-code propagation via
  `PPR_RETURN_ERROR_ON_FAIL`.
- `CMakeLists.txt` — `add_executable(app.game main.cpp)` + `setup_ppr_project(... engine.core/app/math/shader/rhi)`;
  two POST_BUILD steps: (1) copy `$<TARGET_RUNTIME_DLLS:app.game>` (transitive Windows DLLs, e.g. Slang) next to the
  exe — replaces any hardcoded DLL list; (2) copy `assets/shaders` → `<exe-dir>/shaders`.
- Imports only four engine modules (`engine.core/math/rhi/app`) + `imgui_internal` + `std` — `engine.shader` arrives
  transitively via `engine.app`/`engine.rhi`, not via a direct import in `main.cpp`.

## Flow

1. `main()` → `TurboLarbin(ApplicationDomain{}, "ppr", argv span)` → `run()` (dirs, platform/services, window, renderer, camera, UI).
2. Loop: engine `update()` + game `update()` override (demo window) → engine `render()` per frame until cancel/exit.
3. POST_BUILD assets (DLLs + `shaders/`) must land next to the exe or startup file loads fail.

## Integration

- **Consumers**: run configuration / built `app.game` binary.
- **Depends on**: link-time all five engine libs (core/app/math/shader/rhi) but imports four; DearImGui module
  bindings; runtime `shaders/` asset dir + copied DLLs.
- **Provides**: process entry point only — no library, no reusable namespace.

## Key Files

- `main.cpp` — `TurboLarbin` subclass + `main()`.
- `CMakeLists.txt` — `app.game` target, DLL + shader POST_BUILD copies.
