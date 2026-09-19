# lib/engine/

## Responsibility

The PPR engine library tree. Hosts the five C++20 module libraries that compose the engine, organized by
dependency layer: `engine.core` (foundation) → `engine.math` (vector math) → `engine.shader` (Slang
compilation) → `engine.rhi` (GPU abstraction) → `engine.app` (application layer).
`engine.core` is the umbrella foundation re-exporting ~30 partitions into a single `pP` namespace
(`import engine.core;`). `engine.app` is the single compile-time aggregation point (`import engine.app;`):
slim `Application` lifecycle base + `ApplicationEditor` interactive-client subclass owning window, viewport,
input, player, camera, triangle pass, and ImGui service via `IClientService`.

## Design

- **Layered acyclic dependency chain** (mirrored in each `CMakeLists.txt` via `setup_ppr_project`, no upward
  or cyclic edges): `engine.app` → core + math + shader + rhi (+ `glfw`, `mango` private, `imgui.base`/`imgui`
  public); `engine.rhi` → core + math + shader (public) + `slang-rhi`/`slang` (public); `engine.shader` → core
  (public) + `slang` (private); `engine.math` → core (public) + `mango` (private); `engine.core` → `rapidhash`
  (private) + per-platform HAL sources (11 shared areas + windows-only `Random`/`RingBuffer`).
- **Module convention**: `.cppm` = interface (exports), `.cpp` = implementation; single-module libraries
  (`math`, `shader`, `rhi`) vs partitioned umbrellas (`core`, `app`); implementations use `module engine.<lib>;`
  + `import :partition;`. App umbrella `App.cppm` only re-exports (`:application` + `:application_editor`,
  `:input.*`, `:player` + `:player.graph`, `:scene.camera` + controller, five `:service.*`, `:window.*`,
  `:platform`, `:renderer` + triangle_pass + types, `:ui.imgui`).
- **Application lifecycle types**: `ApplicationDomain` immutable-after-construction bitfield
  (`m_is_headless`/`m_is_interactive`/`m_needs_presence`/`m_needs_rendering`/`m_needs_user_interface`);
  `Application` (`:application`) is a `safe_object` subclass with virtual
  `initialize/update/render/shutdown` returning `std::error_code`, owning `unique_ptr<Renderer>`,
  `TimerExplicitClock`, `const unique_ptr<IPlatform>` ordered before `ServicesStore` for safe reverse
  destruction, `SharedContext m_lifecycle` + cancel clause, single-use torn-down latch, args/name/domain/four
  directory entries. `ApplicationEditor` (`:application_editor`, `public Application, protected IClientService`)
  owns `Player`/`Camera`/`ICameraController`/`WindowInputContext`/`InputMapping`/`IUIService`/`WindowViewport`/
   `TrianglePass` with listener `EInputListenerPriority { ui, detector, player }` + mapping
   `EInputMappingPriority::camera` ordering the input chain.
  `App.TemplateInstantiations.cpp` pins explicit `Delegate`/`BroadcastCallback` instantiations.
- **Service locator**: `IService : safe_object` base (no UID member) → `ServicesStore`
  (`FlatMap<type_index, safe_ptr<IService>>` keyed by `typeid(T)`, `shared_mutex`, parent-chain fallback on
  `tryGet`) → `ServiceInjector` implicit DI; per-viewport child stores chain to the root. App services are five
  contracts: `service.client/input/player/ui/window`. Lifetime checked by `safe_ptr` (debug assert, release raw).
- **Foundation details (core)**: strong `Numeric<T,TagT>` + integer aliases/literals/sentinels; rapidhash-backed
  `hash_t` + seedable range combiners + consteval FNV-1a + `Memoizer`; `opaque::Value`/`Block`/`Unique`/`Dict`;
  async `Log::Handler` (~2 MiB `RawChannel`, mutex-guarded policy, `std::jthread` drain, verbosity gate);
  heap-based `TimerManager` (`TimerExplicitClock` + `mainTimer()` backing deadlines); `UnitTest` tree (macros in
  `pP/UnitTest.h`); HAL partition also holds shared helpers (`simd_128_t`, `hash::mix`/`combine`, `overloaded`,
  `Deferred`/`defer`, RNG).
- **Matrix convention**: Mango-native left-handed view space (+Z forward, +Y up), row-major, row-vector
  `mul(float4, matrix)`; Slang sessions fix `SLANG_MATRIX_LAYOUT_ROW_MAJOR`; RHI projections use
  `orthoD3D`/`perspectiveD3D` with [0,1] depth, shared untransposed by all backends.

## Flow

`game/main.cpp` (`app.game`: core + app + math + shader + rhi) → `import engine.app` pulls all five modules
transitively → concrete `Application`/`ApplicationEditor` constructs → `run()` (torn-down guard →
`initialize()` → `PPR_DEFER shutdown()` → loop `while (not m_lifecycle->error())`: clock tick, sleep-throttle
to target frame duration, `update(dt)` + `render()` with first-error-wins exit via request-exit clause):
platform init, directory resolution (`content/install` = executable parent, `working` = `current_path`),
shader/RHI/`Renderer` bootstrap when `ApplicationDomain.m_needs_rendering`, then editor window/viewport/player/
camera/triangle/UI setup → per-frame update (`TimerManager` tick + platform update → viewport sync →
camera model → triangle-pass update → UI update) and render (`renderer.renderAndPresent` over triangle pass +
UI service); shutdown unwinds UI + triangle + renderer, then RHI + shader, then platform last.
Underpinning core flows: allocation via `GPA` → `OS` → `PagePool` → `HugePage`/`SmallPage` → `Arena`/`ScratchPad`;
messaging via `producerReserve`/`producerSubmit` → `consumerAcquire`/`consumerRelease` with
`Signal`/`select` multiplexing and `IContext` cancellation; IO via `IoPort::open`/`read`/`write` +
`pollCompletions`/`waitForCompletions` and `mapFile`/`DirectoryWatcher`; diagnostics via
`PPR_ASSERT/VERIFY/ENSURE` + async logger drain.

## Integration

- Consumed by: `game/main.cpp`, test targets (`engine.tests.core` GLFW-free, `engine.tests.app` GLFW-dependent
  sharing the `engine.tests` static lib `parseCli`/`runSuite`).
- Depends on: third-party libraries (Slang, Slang-RHI, mango::math, GLFW, DearImGui `imgui.base`/`imgui`,
  rapidhash, STB) via `cmake/external/` + `setup_ppr_project`. Lower layers never depend upward, preserving
  acyclicity.
- Provides: transitive `import engine.app` / `import engine.core` for all engine consumers; `Application`
  lifecycle + service-store accessors and `ApplicationEditor` client ownership via `IClientService`; RHI behind
  `IRhiService` and Slang compilation behind `IShaderService` reading sources through `io::mapFile`.

## Key Files

- `core/` — foundation library (types, memory, containers, concurrency, IO, HAL).
  See [core/codemap.md](core/codemap.md).
- `math/` — mango re-export, constants, math helpers (`Math.cppm` only). See [math/codemap.md](math/codemap.md).
- `shader/` — Slang session + module loading. See [shader/codemap.md](shader/codemap.md).
- `rhi/` — GPU abstraction + projections + `IRhiService`. See [rhi/codemap.md](rhi/codemap.md).
- `app/` — application layer (slim Application, ApplicationEditor client, input, window, player, renderer, scene, UI,
  platform, five service contracts). See [app/codemap.md](app/codemap.md).
