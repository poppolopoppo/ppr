# Repository Atlas: PPR Game Engine

This is a navigation aid, not the engineering contract. Source and CMake files
are authoritative; refresh this map when paths, public module boundaries, or
entry points change. For durable architecture and coding policy, see
[AGENTS.md](AGENTS.md). Generated and dependency trees (`out/`, `_deps/`,
`vcpkg_installed/`, `cmake-build-*/`, `build/`, and
`imgui_module_bindings/`) are intentionally out of scope.

## Project Responsibility

A real-time C++23 engine built on C++20 modules, with nine production libraries. Core supplies allocators, containers, concurrency, I/O, configuration and HAL; shader/RHI supply Slang-based GPU infrastructure. The game reference client exercises seeded colony simulation, physics, tools and generated rendering.

`Application` owns lifecycle/platform/services/renderer and the render worker. `ApplicationEditor` owns window/input/player/camera/viewport, grid/triangle passes and UI. Interactive rendering defaults to the worker; headless hosts render inline. Game code captures owned simulation/pass payloads and acknowledges edits from matching consumed results. Kenney assets remain staged but the interactive demo draws generated grid/debug content rather than a committed fixture scene.

## System Entry Points

- [game/main.cpp](game/main.cpp) calls `runGame` in [Game.Application.cpp](game/Game.Application.cpp): typed invocation and configuration resolution, startup validation, smoke dispatch, or `demo::TurboLarbin` interactive client. `--help` is generated from current declarations. `app.run()` errors are printed and returned; exceptions escaping that boundary enter `hal::fatalExit`. See [game map](game/codemap.md).
- [ColonySmoke.cpp](game/colony/ColonySmoke.cpp): headless GPU readback and simulation checks.
- [CMakeLists.txt](CMakeLists.txt) and [CMakePresets.json](CMakePresets.json): target graph, global options and configure presets. `msvc-dev` is diagnostic/ASan; `msvc-live` is Debug Edit & Continue without sanitizers/cache; `msvc-rel` disables tests; `msvc-profile` inherits release and adds profiling/EMPHASIS logging. Hidden gcc presets do not support modules. See [cmake map](cmake/codemap.md) and [build-system](.opencode/skills/build-system/SKILL.md) for toolchain, dependency and execution procedures.
- [include/pP/Macros.h](include/pP/Macros.h): public macro header for build detection, compiler attributes, assertions, logging, error propagation and RAII helpers. `PPR_LOGGING_LEVEL` selects the compiled severity threshold; `OFF` removes all severities. Runtime visibility and backpressure live in `Core.Logger.*`. Old successful runs and compiler-failure hypotheses do not establish current validation status.
- [vcpkg.json](vcpkg.json) and [cmake/external](cmake/external/codemap.md): dependency manifest and package/CPM fallback wiring.
- [Documentation guide](docs/README.md): current references versus design proposals, historical plans and diagrams.

## Architecture Overview

Module dependencies (flat fan-out, per CMake):

```
game/main.cpp → game.application → game.config / game.display / game.colony.*
              → engine.app + engine.core + engine.math + engine.sim (app.game; plus PRIVATE engine.physics per Slice 4 Oracle grant — box2d stays behind engine.physics)
                  engine.app → engine.core / engine.math / engine.shader / engine.rhi / engine.image / engine.mesh
                 engine.rhi → engine.core / engine.math / engine.shader
                 engine.sim → engine.core / engine.math
                 engine.physics → engine.core / engine.math (+ PRIVATE box2d::box2d)
                  engine.shader → engine.core
                  engine.math → engine.core
                  engine.image → engine.core / engine.math (+ PRIVATE mango-image)
                  engine.mesh → engine.core / engine.math (+ PRIVATE mango-import3d)
```

- **Service Locator**: `IService` → `FlatMap<std::type_index, safe_ptr<IService>>` keyed by `typeid(T)` (`Core.Service.cppm:27-83`) → `ServicesStore` with parent-chain fallback →
  `ServiceInjector`. The store supports an explicitly supplied parent; current ImGui code does not own an `m_ui_services` child store.
- **Allocator Composition**: `TAllocator` → `TOwningAllocator` → `TBlockAllocator` → `TArenaAllocator`; concrete
  allocators composed via `InSitu`, `Fallback`, `Threshold`, `Pooling`, `LocalCache`, `HintedPooling`; wrapped by
  `Allocator<A>`, `PMR`, `STL<A>`.
- **Concurrency**: `RawChannel` (lock-free MPSC), `Signal<Events...>` / `select()` (compile-time event multiplexing),
  `IContext`/`SharedContext` (Go-style cancellation tree).
- **Matrix Convention**: Mango-native left-handed view space (+Z forward, +Y up), row-major storage, row-vector
  `mul(float4, matrix)`, and common [0,1] depth. Set at the Slang session level (`SLANG_MATRIX_LAYOUT_ROW_MAJOR`).
- **Application and rendering map**: `ApplicationDomain` describes an app's
  required runtime services and is immutable after construction. Slim
  `Application` owns the run loop + platform/services/shader-RHI-`Renderer`
  bootstrap; `ApplicationEditor` implements `IClientService` and owns scene,
  player, camera, viewport, input-context, triangle pass, and UI state.
  Interactive rendering defaults to the Application-owned dedicated render lane; headless rendering stays inline. Main builds owned frame/pass/UI requests and the owner lane applies them; consumption acknowledgments and GPU completion are distinct. See [frame map](lib/engine/app/frame/codemap.md). `WindowViewport` supplies window geometry; `CameraSnapshot` carries the
  derived viewport size/aspect (`m_viewport_size`/`m_aspect_ratio`). `Renderer`
  consumes `initializer_list<DrawSubmission>` through `render`/`renderToTexture`
  and `initializer_list<SurfaceRenderPass>` through `renderAndPresent`, while
  passes own scene-specific draw resources and encoding.

## Directory Map (Aggregated)

| Directory                       | Responsibility Summary                                                                                | Detailed Map                                        |
|---------------------------------|-------------------------------------------------------------------------------------------------------|-----------------------------------------------------|
| `lib/engine/core/`              | Umbrella foundation: types, memory, containers, concurrency, IO, configuration, services, opaque, HAL.               | [View Map](lib/engine/core/codemap.md)              |
| `lib/engine/core/config/` | Typed schemas, resolution/provenance, CLI/environment and TOML adapters. | [View Map](lib/engine/core/config/codemap.md) |
| `lib/engine/core/memory/`       | Allocator concepts, page pools, Arena/ScopedArena/ScratchPad, composite allocators, PMR, STL, poison. | [View Map](lib/engine/core/memory/codemap.md)       |
| `lib/engine/core/containers/`   | Stack/RingBuffer, Sparse/StableVector, HashMap/HashSet/FlatMap, Bitmask, views, RelPtr/TagPtr.        | [View Map](lib/engine/core/containers/codemap.md)   |
| `lib/engine/core/concurrency/`  | RawChannel (lock-free MPSC), Signal/select, IContext cancellation tree.                               | [View Map](lib/engine/core/concurrency/codemap.md)  |
| `lib/engine/core/io/`           | IoPort async I/O, MappedFile, DirectoryWatcher, IoEvent/IoResult.                                     | [View Map](lib/engine/core/io/codemap.md)           |
| `lib/engine/core/function/`     | std23::function_ref, Delegate/BroadcastCallback + Handle dispatch.                                    | [View Map](lib/engine/core/function/codemap.md)     |
| `lib/engine/core/hal/`          | HAL umbrella: 10 areas — page memory, ring buffer, I/O (map/watch), process, timers, native transcoding, filesystem, debugger + profiling markers, system/RNG. `Random`/`RingBuffer` exist only on windows + linux. | [View Map](lib/engine/core/hal/codemap.md)          |
| `lib/engine/core/hal/windows/`  | Win32 HAL: VirtualAlloc2, IOCP, ReadDirectoryChangesW, CreateProcessW.                                | [View Map](lib/engine/core/hal/windows/codemap.md)  |
| `lib/engine/core/hal/linux/`    | POSIX HAL: mmap/mprotect, inotify, fork+execvp, timer_create, raw-syscall io_uring, memfd ring buffer. | [View Map](lib/engine/core/hal/linux/codemap.md)    |
| `lib/engine/core/hal/darwin/`   | XNU HAL: mmap/MAP_ANON, fork+execvp, Mach sysctl debugger.                                            | [View Map](lib/engine/core/hal/darwin/codemap.md)   |
| `lib/engine/core/hal/generic/`  | Stub HAL: throw/no-op fallback for any platform.                                                      | [View Map](lib/engine/core/hal/generic/codemap.md)  |
| `lib/engine/math/`              | Single-module mango::math re-export into `namespace pP` + math:: utilities.                           | [View Map](lib/engine/math/codemap.md)              |
| `lib/engine/image/`             | CPU image assets (`:types` frozen §2.2 vocabulary + `:decode` PNG/JPG/KTX2/DDS via private Mango). | [View Map](lib/engine/image/codemap.md)             |
| `lib/engine/mesh/`              | CPU static mesh assets (`:types` frozen vocabulary + `:convert` glTF/GLB import).      | [View Map](lib/engine/mesh/codemap.md)              |
| `lib/engine/rhi/`               | Wraps Slang-RHI: GPU types, common projection helpers, IRhiService.                                   | [View Map](lib/engine/rhi/codemap.md)               |
| `lib/engine/shader/`            | Wraps Slang: IShaderService, SharedModule, row-major session.                                         | [View Map](lib/engine/shader/codemap.md)            |
| `lib/engine/sim/`               | Deterministic simulation: 4096x4096-cell world as 32x32 chunks of 128x128 (1024 chunks), fixed-timestep tick driver with a step registry, versioned snapshot save/load, ECS columns, footprint, worldgen. Renderer-independent.  | [View Map](lib/engine/sim/codemap.md)               |
| `lib/engine/physics/`           | Box2D boundary — box2d types never leak past it: `Scene` world, indexed/diffed `ChunkColliders` rebuild, `Coupling` publish (used by `engine.tests.sim` only). | [View Map](lib/engine/physics/codemap.md)           |
| `lib/engine/app/`               | Application umbrella: slim Application base + ApplicationEditor subclass (IClientService), re-exports all app submodules. | [View Map](lib/engine/app/codemap.md)               |
| `lib/engine/app/input/`         | Input action/key/listener + FilteredAnalog/device message layer + Routing background-drag latch (12 files, 6 partitions).                      | [View Map](lib/engine/app/input/codemap.md)         |
| `lib/engine/app/platform/`      | IPlatform 9-method interface + create() factory, platform errc/version helpers.                        | [View Map](lib/engine/app/platform/codemap.md)      |
| `lib/engine/app/platform/glfw/` | GLFW backend: IPlatform + IInputService + IPlayerService + IWindowService.                            | [View Map](lib/engine/app/platform/glfw/codemap.md) |
| `lib/engine/app/player/`        | IPlayerService, Player::Graph state machine.                                                          | [View Map](lib/engine/app/player/codemap.md)        |
| `lib/engine/app/renderer/`      | Content-free Renderer (surfaces, queue, submission) + bindless TrianglePass (pass-owned GPU caches); boundary Types header-only (.cppm, no Types.cpp). | [View Map](lib/engine/app/renderer/codemap.md)      |
| `lib/engine/app/scene/`         | Camera + controller (lookat view, view*projection snapshot).                                          | [View Map](lib/engine/app/scene/codemap.md)         |
| `lib/engine/app/service/`       | Five app service contracts: client/input/player/ui/window (behavior in platform/UI/Editor).            | [View Map](lib/engine/app/service/codemap.md)       |
| `lib/engine/app/ui/`            | UI layer (ImGui integration, IUIService; overlay shader embedded in `App.UI.ImGui.cpp`, not an asset). | [View Map](lib/engine/app/ui/codemap.md)            |
| `lib/engine/app/window/`        | IWindowService lifecycle (Handle/Monitor) + Viewport geometry (moved from renderer).                                   | [View Map](lib/engine/app/window/codemap.md)        |
| `cmake/`                        | Root CMake: presets, compilers, sanitizers, dependencies.                                             | [View Map](cmake/codemap.md)                        |
| `cmake/compiler/`               | Per-compiler flag config (MSVC, Clang, GCC, sanitizers).                                              | [View Map](cmake/compiler/codemap.md)               |
| `cmake/external/`               | External dependency CMake (CPM/vcpkg: GLFW, Mango, rapidhash, SlangRHI, STB, DearImGui).                                     | [View Map](cmake/external/codemap.md)               |
| `game/`                         | Demo exe (`app.game`): editor-driven generated colony with `GridPass` + panel and a separate headless `--smoke` path; Every build synchronizes shaders/textures/meshes; POST_BUILD stages DLLs where configured. | [View Map](game/codemap.md)                         |
| `include/pP/`                   | Single public non-module header `Macros.h` (build-mode/poison detection, attributes, assertions, logging, error returns, RAII helpers). | [View Map](include/pP/codemap.md)                   |
| `assets/`                       | Runtime asset root: shaders + textures + meshes; Slang sources compiled at startup by `engine.shader`.                 | [View Map](assets/codemap.md)                       |
| `assets/shaders/`               | Slang shader sources, compiled at startup by `engine.shader`: `mesh_bindless.slang` (`TrianglePass`) and `grid_tiles.slang` (`GridPass`). | [View Map](assets/shaders/codemap.md)               |
| `lib/`                          | Umbrella for `lib/engine/*`; module graph overview.                                                            | [View Map](lib/codemap.md)                          |
| `lib/engine/`                   | Umbrella for the engine modules; authoritative module inventory + dependency edges.                          | [View Map](lib/engine/codemap.md)                   |
| `include/`                      | Umbrella for the single public non-module header.                                                             | [View Map](include/codemap.md)                      |
| `lib/engine/tests/asset/`       | Asset-pipeline test suite (image/mesh/staging/gpu/gate).                                                     | [View Map](lib/engine/tests/asset/codemap.md)       |

> Removed locations (do not look here): `lib/engine/app/camera/` → moved to `lib/engine/app/scene/`;
> `lib/engine/app/input/device/` → consolidated into the unified `:input.device` partition; renderer `App.Viewport`
> → moved to `:window.viewport`; `App.Input.Mapping`/`App.Input.Replay` partitions deleted (mapping merged into
> `:input.action`).
>
> On disk but NOT built: `lib/engine/app/scene/App.Scene.World.cppm` (`IWorld` +
> `WorldHitResult`) is in neither `lib/engine/app/CMakeLists.txt` nor the
> `App.cppm` re-export list, so it is dead code today. Its presence does not make it a supported API or authorize unrelated cleanup.

## Test Infrastructure

Tests exist unless `BUILD_TESTING` is explicitly OFF (`msvc-rel` and inherited `msvc-profile`). `PPR_TEST_LOOPS` defaults to 1 even under ASan; configure 3 for stress and restore 1 explicitly when reusing the build directory.

Current CMake registers twelve entries: `EngineCoreUnitTests`, `EngineSimUnitTests`, `EngineAssetUnitTests`, `GameConfigUnitTests`, `WorldgenValidateFrontier`, `WorldgenValidateHollows`, and six app shards: `app_a1_death_lifecycle`, `app_a2_gates`, `app_a3_display_window`, `app_b_sim`, `app_c_fence`, `app_d_io`. The two profile entries perform CPU-only pack load/compile validation. All app shards and asset use `RUN_SERIAL TRUE`; `ctest --test-dir out/build/msvc-dev -j4 --output-on-failure` honors those exclusions. Only one independent matrix invocation per machine may run at once, without competing profiling/game workloads. Core/sim are GLFW-free; app and GPU asset tiers require platform/GPU facilities.

- `lib/engine/tests/CMakeLists.txt`: shared loop setting, engine suites, aggregate engine-test target. The aggregate does not run game tests; full CTest includes them.
- `lib/engine/tests/app/CMakeLists.txt`: exact shard filters, timeouts and serialization. Filter matching is prefix-based both ways; use complete group paths (`app/player/` avoids sibling matches) and full run-gate leaf tokens. App a3 isolates display/window transactions.
- `lib/engine/tests/core/`: memory, containers, concurrency, I/O, configuration, services and other foundation behavior.
- `lib/engine/tests/sim/`: chunks, fixed timestep, snapshots, ECS, world generation, physics; physics links privately.
- [asset tests](lib/engine/tests/asset/codemap.md): CPU image/mesh processing and GPU caches/render integration.
- `game/CMakeLists.txt`: `game.tests` / `GameConfigUnitTests`, covering invocation and game display model/persistence without an upward engine-test dependency.
- `lib/engine/tests/shared/`: shared invocation/run harness. Tests use `PPR_TEST_ASSERT`, not engine assumptions/assertions.

Suite interfaces export declaration-only `extern const UnitTest` roots; implementations assemble private groups through `Tests()` accessors. Use `unit-test-updater` for test changes and owning CMake files for current file/group inventories rather than old counts.

Validation scope and review timing are defined by [validation](.opencode/skills/validation/SKILL.md) and [build-system](.opencode/skills/build-system/SKILL.md). Local batches default to an affected Dev build and focused behavioral checks; coherent completion, shared render transactions, and merge/stress gates expand coverage as specified there. Reuse applicable passed evidence and rerun only invalidated checks. Documentation and script changes use proportionate reference, syntax, and behavior checks rather than unrelated C++ builds or CTest. No automatic retries masking flakes; preserve failure seeds.

## Map maintenance

Keep this document focused on finding code. Update links and summaries with
their corresponding source changes; put durable rules in `AGENTS.md` instead.

- Diagrams: rendered PlantUML galleries under `docs/diagrams/` — `app-game-engine-arch/`, `engine.app/`, `readme/`, and `build-report/` (measured build costs, critical-path table, and Ninja graph downloads).
