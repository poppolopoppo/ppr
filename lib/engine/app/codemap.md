# lib/engine/app

## Responsibility

`engine.app` is the application-layer umbrella module: the single compile-time aggregation point (`import engine.app;`)
for the full stack — application lifecycle, input, player, scene camera, services, window, platform, renderer, and UI.
`pP::Application` owns the main loop, directory resolution, service stores, main window/viewport, renderer + triangle
pass, and the owned scene camera stack. `game/main.cpp` subclasses it (`TurboLarbin`) and drives `run()`.

## Design

- Umbrella `App.cppm` only `export import`s partitions — `:application`, `:input.*` (5), `:player` + `:player.graph`,
  `:scene.camera` + `:scene.camera.controller`, `:service.*` (input/player/ui/window), `:window.*`
  (viewport/handle/monitor), `:platform`, `:renderer` + `:renderer.triangle_pass` + `:renderer.types`, `:ui.imgui`.
- `Application` (`App.Application.cppm/.cpp`, `module engine.app:application`) — `safe_object` subclass with virtual
  `initialize/update/render/shutdown` hooks returning `std::error_code`; lifecycle `SharedContext` + cancel func;
  hot per-frame state (cached window/input service pointers, `WindowInputContext`, scene `InputListener` +
  `InputMapping`, owned `Camera`/`CameraModel`/`FreeCameraController`, viewport, renderer, triangle pass) vs cold
  init-time state (service stores, window callbacks, `IUIService`, platform, dirs).
- DELETED: old top-level `camera/` directory is gone — camera now lives only under `scene/` (`:scene.camera`,
  `:scene.camera.controller`, registered in CMakeLists). No `camera/` partition remains in the umbrella.
- `App.TemplateInstantiations.cpp` pins explicit instantiations for `Delegate` (window events) and
  `BroadcastCallback` (monitor/window/input/player/time) so downstream users don't pay implicit-instantiation cost.
- CMakeLists registers all `.cppm` (FILE_SET CXX_MODULES) + `.cpp` privately; links `engine.core/math/shader/rhi`
  public-internal, `glfw`/`mango` private, `imgui.base` + `imgui` public (so `import imgui;` resolves for importers).

## Flow

1. `game/main.cpp` → `TurboLarbin : Application` → `app.run()` → `initialize()` (lifecycle context, platform init,
   dirs, window/viewport, renderer + triangle pass, scene camera + controller mapping, UI service) → loop
   `update()` → `render()` until lifecycle errors/cancel → `shutdown()` via `PPR_DEFER` (throws `system_error` on
   failure outside unwind).
2. Per-frame `update()` polls window events, posts input messages, ticks the scene controller into the owned camera;
   `render()` builds stack `DrawSubmission`s and submits via `Renderer::renderAndPresent` / `submitToTexture`.
3. No runtime logic in `App.cppm` itself — compile-time re-export only.

## Integration

- **Consumers**: `game/main.cpp` (sole app subclass), `engine.tests.app` (platform tests).
- **Depends on**: `engine.core` (services, context, delegates), `engine.math`, `engine.rhi`, `engine.shader`
  (via renderer/UI pass), external `glfw`/`mango`/`imgui`.
- **Provides**: every `engine.app:*` namespace; `Application` lifecycle + service-store accessors
  (`getServices()`, `getUiServices()`, `getMainWindow()`, `getLifecycle()`).

## Key Files

- `App.cppm` — umbrella re-export list (no runtime code).
- `App.Application.cppm/.cpp` — `Application` interface + lifecycle/loop implementation.
- `App.TemplateInstantiations.cpp` — explicit `Delegate`/`BroadcastCallback` instantiations.
- `CMakeLists.txt` — module + source registration, `setup_ppr_project` deps.
