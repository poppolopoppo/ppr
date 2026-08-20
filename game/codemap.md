# game/

## Responsibility
Application entry point for the PPR engine demo. Hosts the top-level `main()` that constructs the engine `Application` subclass and drives the run loop, plus the CMake target (`VideoGameApp`) that links the engine modules and copies shader assets post-build.

## Design
- `main.cpp` defines `demo::TurboLarbin`, a subclass of `pP::Application`, overriding the lifecycle hooks `initialize()` / `update()` / `shutdown()` (each returning `std::error_code`).
- Uses `PPR_DEFINE_LOG_CATEGORY(Demo, info, none)` for a scoped logging category and `PPR_RETURN_ERROR_ON_FAIL` for error-propagation guards.
- `update()` conditionally shows an ImGui demo window in debug builds via `getUiServices().get<IUIService>()`.
- `main()` builds the app with `demo::TurboLarbin app("ppr", std::span(&argv[0], argc))` and returns `app.run().value()` as the process exit code.

## Flow
1. `main()` → constructs `TurboLarbin` → `app.run()` enters the engine loop.
2. Engine resolves install/config/content/working directories and discovers registered services (input, window, player, RHI, shader).
3. `run()` calls `initialize()` once, then loops `update()` (user hook) → `render()` per frame until exit is requested, then `shutdown()` (via `PPR_DEFER`).
4. `game/CMakeLists.txt` builds `VideoGameApp` via `setup_ppr_project` and copies `assets/shaders` POST_BUILD.

## Integration
- Imports `engine.core`, `engine.math`, `engine.rhi`, `engine.app`, `imgui_internal`, `std`.
- Consumed by: the run configuration / `VideoGameApp` executable.
- Depends on: all five engine modules and the DearImGui module bindings.

## Key Files
- `main.cpp` — entry point, `TurboLarbin` Application subclass, `main()`.
- `CMakeLists.txt` — `VideoGameApp` target, `setup_ppr_project`, shader asset copy.
