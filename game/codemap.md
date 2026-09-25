# game/

## Responsibility

Demo executable (`app.game`): `demo::TurboLarbin : ApplicationEditor` privately owns a static Kenney colony fixture, plus `main()` that constructs it (`"ppr"`, argv span via `pP::checked_cast<std::size_t>(argc)` — `int`→`size_t` sign-conversion safe under Clang `-Wconversion -Werror`) and returns `app.run().value()` as the exit code. The fixture imports 16 staged GLBs with `engine.mesh`, decodes their image references with `engine.image`, uploads each scene to the editor-owned `TrianglePass`, and submits a 21-placement cutaway habitat without using `ApplicationEditor::loadScene` or `unloadScene`. The editor partition SHIPS — `main.cpp` imports `ApplicationEditor` unconditionally, so do not attempt to gate it behind `BUILD_TESTING`/`PPR_ENABLE_UNIT_TEST`. It hosts the `Application` run loop without owning engine behavior: scene, player, camera, viewport, and UI state are inherited from `ApplicationEditor`/`Application`; game code adds fixture lifecycle ownership and a debug-only ImGui demo window.

## Design

- Patterns/abstractions: template-method lifecycle hooks (`initialize()` / `update(const TimeSpan)` / `shutdown()` returning `std::error_code`), inherited constructors (`using super_t::super_t` where `super_t = ApplicationEditor`), data-driven static asset and placement tables, per-entry CPU/GPU ownership, reverse receipt release, service-locator lookup with validity guard (`getServices().get<IUIService>()` + `isValid()`), and thin `main` glue delegating to `Application::run()`.
- `main.cpp` — `TurboLarbin` owns 16 loaded `LoadedAsset` entries and a spec-to-loaded mapping. `initialize` first initializes the editor, then imports `meshes/kenney_colony` with `mesh::importAndConvert`, resolves each `ImageRef`, decodes RGBA8 color images, and calls `TrianglePass::uploadScene` without mip generation. `update` first updates the editor, clears pass instances, walks the named 21-placement habitat layout's `SceneInstance -> mesh -> primitive` source descriptors, and submits `placement * source_world` with row-major row-vector transforms. `shutdown` disables submissions, releases successful receipts in reverse order, preserves the first cleanup error, then shuts down the editor. `PPR_DEFINE_LOG_CATEGORY(Demo, …)` provides load, one-time submission, and release logging without per-frame spam.
- `CMakeLists.txt` — `add_executable(app.game main.cpp)` + `setup_ppr_project(... engine.core/app/math/shader/rhi)`; two POST_BUILD steps: (1) copy `$<TARGET_RUNTIME_DLLS:app.game>` (transitive Windows DLLs, e.g. Slang) next to the exe — replaces any hardcoded DLL list — guarded by `if(WIN32)` (the genex is empty on Linux, which would degrade the copy command into a usage error); (2) unconditional copy `assets/shaders` → `<exe-dir>/shaders`.
- Imports six engine C++23 modules (`engine.core/image/math/mesh/rhi/app`) + `imgui_internal` + `std`; direct `engine.image` and `engine.mesh` imports expose the colony fixture APIs.

## Flow

1. `main(argc, argv)` → `TurboLarbin("ppr", std::span(&argv[0], checked_cast<size_t>(argc)))` → `app.run()` (dirs, platform/services, window, renderer, camera, UI bootstrap owned by `Application`/`ApplicationEditor`).
2. `run()` → `TurboLarbin::initialize()` → `ApplicationEditor::initialize()` → static GLB import/image decode/GPU upload for the staged colony.
3. Loop per frame until cancel/exit: `ApplicationEditor::update(dt)` → `TurboLarbin::update(dt)` rebuilds the colony submissions → debug-only ImGui demo window → engine render.
4. Teardown: `TurboLarbin::shutdown()` clears instances and releases fixture receipts in reverse, then `ApplicationEditor::shutdown()`; `main` returns `err.value()`.
5. POST_BUILD assets (Windows-only DLLs + shaders/textures/meshes) must land next to the exe or startup file loads fail.

## Integration

- **Consumers**: run configuration / built `app.game` binary.
- **Depends on**: engine app/core/image/math/mesh/rhi modules; `ApplicationEditor` + `Application` run loop + `IUIService`; `mesh::importAndConvert`; `image::decodeToRgba8`; `TrianglePass`; DearImGui bindings; runtime `shaders/`, `textures/`, and `meshes/kenney_colony/` staged beside the executable.
- **Provides**: process entry point only — no library, no reusable namespace.

## Key Files

- `main.cpp` — `TurboLarbin` subclass + `main()`.
- `CMakeLists.txt` — `app.game` target, DLL + shader POST_BUILD copies.
