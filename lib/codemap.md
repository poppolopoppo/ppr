# lib/

## Responsibility

Source root for the PPR engine libraries. All engine code lives under `lib/engine/` as C++20 modules
(`engine.core`, `engine.math`, `engine.shader`, `engine.rhi`, `engine.app`); test infrastructure lives in
`lib/engine/tests/` (excluded from this codemap per skill rules).
`engine.core` is the umbrella foundation re-exporting ~30 partitions into a single `pP` namespace
(`import engine.core;`). `engine.app` is the single compile-time aggregation point (`import engine.app;`):
slim `Application` lifecycle base + `ApplicationEditor` interactive-client subclass owning window, viewport,
input, player, camera, triangle pass, and ImGui service via `IClientService`.

## Design

- Single library tree: `lib/engine/` holds the five engine modules plus the test suite.
- Layered acyclic dependency chain (mirrored in each `CMakeLists.txt` via `setup_ppr_project`, no upward or
  cyclic edges): `engine.app` → core + math + shader + rhi (+ `glfw`, `mango` private, `imgui.base`/`imgui`
  public); `engine.rhi` → core + math + shader (public) + `slang-rhi`/`slang` (public); `engine.shader` → core
  (public) + `slang` (private); `engine.math` → core (public) + `mango` (private); `engine.core` → `rapidhash`
  (private) + per-platform HAL sources (11 shared areas + windows-only `Random`/`RingBuffer`).
- Module convention: `.cppm` = interface (exports), `.cpp` = implementation; single-module libraries
  (`math`, `shader`, `rhi`) vs partitioned umbrellas (`core`, `app`); implementations use `module engine.<lib>;`
  + `import :partition;`. App umbrella `App.cppm` only re-exports (`:application` + `:application_editor`,
  `:input.*`, `:player`, `:scene.camera`, five `:service.*`, `:window.*`, `:platform`, `:renderer` + triangle
  pass, `:ui.imgui`).
- Application lifecycle types: `ApplicationDomain` immutable-after-construction bitfield
  (`m_is_headless`/`m_is_interactive`/`m_needs_presence`/`m_needs_rendering`/`m_needs_user_interface`);
  `Application` (`:application`) is a `safe_object` subclass with virtual `initialize/update/render/shutdown`
  returning `std::error_code`, owning `Renderer`/`TimerExplicitClock`/`IPlatform`/`ServicesStore`/`SharedContext`
  + torn-down latch. `ApplicationEditor` (`:application_editor`, `Application` + `IClientService`) owns
  player/camera/controller/input-mapping/UI-service/viewport/triangle pass with
  `EInputPriority { ui, camera, player }` ordering the input chain.
- Service locator: `IService : safe_object` → `ServicesStore` (parent-chain fallback) → `ServiceInjector`;
  per-viewport child stores chain to the root. App services are five contracts:
  `service.client/input/player/ui/window`. Lifetime checked by `safe_ptr` (debug assert, release raw).
- Foundation + matrix: strong `Numeric`/hash/`opaque`/async `Log`/`TimerManager`/`UnitTest`/HAL helpers in core;
  Mango-native left-handed view space (+Z forward, +Y up), row-major, row-vector `mul(float4, matrix)`;
  Slang `ROW_MAJOR`; RHI `orthoD3D`/`perspectiveD3D` with [0,1] depth, shared untransposed by all backends.
- `app.game` (`game/`) links all five engine modules; no engine code lives outside `lib/engine/`.

## Flow

`game/main.cpp` → `import engine.app` → umbrella re-exports `engine.core`, `engine.math`, `engine.shader`,
`engine.rhi` → transitive access to all partitions; concrete `Application`/`ApplicationEditor` constructs →
`run()` (torn-down guard → `initialize()` → `PPR_DEFER shutdown()` → loop `while (not m_lifecycle->error())`:
clock tick, sleep-throttle to target frame duration, `update(dt)` + `render()` with first-error-wins exit):
platform init, directory resolution (`content/install` = executable parent, `working` = `current_path`),
shader/RHI/`Renderer` bootstrap when `ApplicationDomain.m_needs_rendering`, then editor window/viewport/player/
camera/triangle/UI setup → per-frame update (`TimerManager` tick + platform update → viewport sync → camera
model → triangle-pass update → UI update) and render (`renderAndPresent` over triangle pass + UI service);
shutdown unwinds UI + triangle + renderer, then RHI + shader, then platform last. Underpinned by allocation
(`GPA` → `OS` → `PagePool` → `Arena`/`ScratchPad`), messaging (`producerReserve`/`Submit` →
`consumerAcquire`/`Release` + `Signal`/`select` + `IContext`), IO (`IoPort` + `mapFile`/`DirectoryWatcher`),
and diagnostics (`PPR_ASSERT/VERIFY/ENSURE` + async logger drain).

## Integration

- Consumed by: `game/` (entry point), CMake build system (`cmake/`), test targets (`engine.tests.core`
  GLFW-free, `engine.tests.app` GLFW-dependent sharing the `engine.tests` static lib `parseCli`/`runSuite`).
- Depends on: third-party libraries (Slang, Slang-RHI, mango::math, GLFW, DearImGui `imgui.base`/`imgui`,
  rapidhash, STB) via `cmake/external/` + `setup_ppr_project`. Lower layers never depend upward, preserving
  acyclicity.
- Provides: transitive `import engine.app` / `import engine.core` for all engine consumers; `Application`
  lifecycle + service-store accessors and `ApplicationEditor` client ownership via `IClientService`; RHI behind
  `IRhiService` and Slang compilation behind `IShaderService` reading sources through `io::mapFile`.
- See [engine/](engine/codemap.md) for the full engine module map.

## Key Files

- `engine/` — all engine module libraries (see [engine/codemap.md](engine/codemap.md)).
