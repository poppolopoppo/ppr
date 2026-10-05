# Repository Atlas: PPR Game Engine

This is a navigation aid, not the engineering contract. Source and CMake files
are authoritative; refresh this map when paths, public module boundaries, or
entry points change. For durable architecture and coding policy, see
[AGENTS.md](AGENTS.md). Generated and dependency trees (`out/`, `_deps/`,
`vcpkg_installed/`, `cmake-build-*/`, `build/`, and
`imgui_module_bindings/`) are intentionally out of scope.

## Project Responsibility

A high-performance, real-time C++23 game engine built on C++20 modules. PPR
provides a flat fan-out foundation, a tiered allocator hierarchy, lock-free
concurrency primitives, a platform HAL (Windows/Linux/Darwin/Generic),
Slang-based shader compilation, and a Slang-RHI GPU abstraction. Slim
`Application` owns the run loop + platform/services/shader-RHI-`Renderer`
bootstrap; interactive `ApplicationEditor` (`IClientService`) owns scene,
player, camera, viewport, input-context, triangle and grid passes, and UI state.
The `game/` demo hosts `TurboLarbin : ApplicationEditor`, which owns seeded
generated-colony simulation, submits resident chunks to the editor-owned
`GridPass`, and presents camera-visible counts in a Colony panel. `--smoke`
instead runs a separate headless, rendering-enabled `Application` for offscreen
GPU readback; the editor keeps its `TrianglePass` initialized and actively fed — a generated unit-quad bag + 2 packed materials (`uploadDebugDraw_`, `game/main.cpp:1788-1834`) with per-frame agent/path/grid/edit instances (`submitDebugInstances_`, `:2177-2284`; agents/paths/grid/edit outlines `:2220-2278`) — but loads no committed-asset fixture scene. Kenney `kenney_colony` assets remain on disk and staged (`assets/meshes/kenney_colony/PROVENANCE.md:1-5`, `game/CMakeLists.txt:57-63`).

## System Entry Points

- `game/main.cpp` — Process entry point; dispatches `--smoke` to `game.colony.smoke::runColonySmoke(argv)` (returns the resulting `std::error_code::value()`), `--load100` to `runLoad100()` and `--time-trials` to `runTimeTrials()` (each returns the error's value on failure, literal `0` on success: `:2800-2816`) before constructing the editor; the `--profile*` flags (`--profile100`, `--profile-csv`, `--profile-label`, `--profile-skip`, `--profile-edits`, `--profile-pan`) plus `--gpu-validation` instead configure the editor path (`:2830-2864`; `--profile-csv`/`--profile-label`/`--profile-skip` each accept space- and `=`-forms). Otherwise `demo::TurboLarbin : ApplicationEditor` initializes a seeded `ColonyDriver`, orthographic pan camera, game input mapping, and `ColonyTranslator`; per frame it drains edit chunks via `presentEdits`, stages generated tiles (`translator.submit`), resolves hover (`refreshHover_`), submits 3D debug instances (`submitDebugInstances_`), and draws the Colony panel plus the ImGui-foreground feedback overlay (`drawFeedbackOverlay_`, `:2081-2175`; frame phases `:2515-2527`). The run loop returns `run_error.value()` after a `[fatal]` print (`:2869-2875`); uncaught `std::exception`/unknown exceptions exit `100`/`101` (`:2876-2881`). Per-stage timings are an observation side-channel, not a module: `StageTimer`/`StageStat` accumulate wall-clock us per `Stage` (`game/colony/StageTiming.h:53-116`) under `hal::ProfileScope` `Colony.*` spans in `ColonyDriver.cpp`/`ColonyTranslator.cpp`; 4 of 19 rows (`CacheHit`/`CacheMiss`/`NeededCells`/`ChunksDrained`, `StageTiming.h:12-18`, classified by `stageIsCounter` in `ColonyPanel.cppm:162-167`) are call-count counters, not timed stages.
- `game/colony/ColonySmoke.cppm` / `game/colony/ColonySmoke.cpp` — Separate headless `Application` host for one offscreen `GridPass` frame and generated-cell GPU readback; no editor window or UI.
- `CMakeLists.txt` + `CMakePresets.json` — Build configuration. Public presets
  include `msvc-dev`, `msvc-live` (EnC: `/ZI` + `/DEBUG:FULL` + `/INCREMENTAL` + `/OPT:NOREF,NOICF` +
  `/LTCG:OFF` + `/PDBTMCACHE`, all `PPR_EDIT_AND_CONTINUE`-scoped; live-only `/MDd`; LNK4075 validators fail
  at configure time), `msvc-rel` (shipping: pinned `/O2` + `/Ob2`, `/GL` + `/LTCG`, `/OPT:REF,ICF`,
  `/INCREMENTAL:NO`, `/DEBUG` + stripped `app.game.stripped.pdb` beside full `app.game.pdb`; Release `/Zi`,
  dev `/Z7`; `BUILD_TESTING OFF`), `msvc-profile` (inherits `msvc-rel`: same `/O2`/`/Ob2`/`/GL`/`/Gw`,
  LTCG, NDEBUG, PDBSTRIPPED + `PPR_ENABLE_PROFILING=ON` (`/DUSE_PIX`, `cmake/compiler/MSVC.cmake:89-94`) +
  `PPR_LOGGING_LEVEL=EMPHASIS`, `CMakePresets.json:118-127`); options `PPR_ENABLE_PROFILING` + `PPR_LOGGING_LEVEL`
  (OFF/DEBUG/VERBOSE/INFO/EMPHASIS/WARNING/ERROR/FATAL → numeric `PPR_LOG_LEVEL_THRESHOLD`, `CMakeLists.txt:69-101`);
  per-preset levels dev/live `DEBUG`, rel `WARNING` (default), profile `EMPHASIS`, `OFF` available; full-`OFF`
  `app.game` unvalidated (suspected Insiders-MSVC IFC-import ICE, repro via git-ignored `CMakeUserPresets.json`
  `msvc-off`, `.gitignore:64`); `OFF` green for core/shader/image/rhi; shipping `WARNING` + profiling `EMPHASIS`
  validated separately. `clang-cl-*`, and `clang-*`;
  `gcc-*` presets are hidden and do not support modules.
- `include/pP/Macros.h` — The single public non-module header (included via `module; #include` in the global
  fragment); engine-wide preprocessor vocabulary: build-mode + memory-poisoning detection, pointer-size
  selection, compiler-attribute portability, wide-char literals, assertions, logging (compile-time
  `PPR_LOG_LEVEL_THRESHOLD`, `OFF`=7 above fatal=6, `:215-244`; caller-side static erasure + `Log::isVisible`
  runtime guard inside `PPR_LOG` (`:266-409`; `Core.Logger.cppm:88`, `Core.Logger.cpp:296-311`; fail-macros inherit
  via `PPR_LOG`/`PPR_ENABLE_LOGGING`, `:549-627`), per-severity ring backpressure (wait at error+, drop below,
  `Core.Logger.cpp:145-203`), no per-frame flush with shutdown drain retained (`App.Application.cpp:78-81`),
  error-propagation returns, RAII helpers.
- `vcpkg.json` — Dependency manifest (vcpkg or CPM fallback).

## Architecture Overview

Module dependencies (flat fan-out, per CMake):

```
game/main.cpp → game.colony.buildings / game.colony.driver / game.colony.translator / game.colony.panel / game.colony.pathfinding / game.colony.smoke / game.colony.timetrials
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

- **Service Locator**: `IService` → compile-time `typeUid<T>()` → `ServicesStore` with parent-chain fallback →
  `ServiceInjector`. Per-viewport child store (`m_ui_services`) chains to the root.
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
  `WindowViewport` supplies window geometry; `CameraSnapshot` carries the
  derived viewport size/aspect (`m_viewport_size`/`m_aspect_ratio`). `Renderer`
  consumes `initializer_list<DrawSubmission>` through `render`/`renderToTexture`
  and `initializer_list<SurfaceRenderPass>` through `renderAndPresent`, while
  passes own scene-specific draw resources and encoding.

## Directory Map (Aggregated)

| Directory                       | Responsibility Summary                                                                                | Detailed Map                                        |
|---------------------------------|-------------------------------------------------------------------------------------------------------|-----------------------------------------------------|
| `lib/engine/core/`              | Umbrella foundation: types, memory, containers, concurrency, IO, services, opaque, HAL.               | [View Map](lib/engine/core/codemap.md)              |
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
| `game/`                         | Demo exe (`app.game`): editor-driven generated colony with `GridPass` + panel and a separate headless `--smoke` path; POST_BUILD stages shaders/textures/meshes (and DLLs where configured). | [View Map](game/codemap.md)                         |
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
> `App.cppm` re-export list, so it is dead code today. Register it (with its
> `.cpp`) or delete it; either way `app/scene` and `app` need a follow-up.

## Test Infrastructure

- Test targets + `Core.UnitTest` partition (`PPR_ENABLE_UNIT_TEST`, conditional `export import :unit_test`)
  sit behind `BUILD_TESTING` (undefined-tolerant: present unless explicitly `OFF`; `msvc-rel` sets `OFF`).
  The editor partition ships — `game/main.cpp` imports `ApplicationEditor` unconditionally.
- `engine.tests.core` (`lib/engine/tests/core/`) — GLFW-free; memory, containers, concurrency, IO, strings, opaque,
  services, enums. Thematic private groups (27 `*Tests.cpp` files: per-area splits such as `Core.Allocator.Tests.cpp`,
  `Core.Memory.Slab/Arena/PagePool.Tests.cpp`, `Core.Containers.*.Tests.cpp`, `Core.Concurrency.*.Tests.cpp`,
   `Core.Enums/Math/Strings/Utility.Tests.cpp`, `Core.Opaque/Service.Tests.cpp` (`16` top-level groups); umbrella exports only `extern const UnitTest core` (decl-only `.cppm` + out-of-line def is the MSVC C1001 workaround).
- `engine.tests.app` (`lib/engine/tests/app/`) — links GLFW for platform-dependent tests. Thematic private groups
  (18 `*Tests.cpp` files: `App.Player/PlayerService/Player.Graph`, `App.Devices/Input.Listener/FilteredAnalog/WindowInput`,
  `App.Shader/Viewport/PixelReadback`, `App.Camera/Quaternion`, `App.ImGuiRouting/ImguiDpi/ZeroVProbe`
  `Tests.cpp` + `App.Tests.cpp` root of 28 nodes; umbrella exports only `extern const UnitTest app` (same MSVC C1001 workaround).
- Group pattern: `module engine.tests.<suite>;` impl unit (PRIVATE SOURCES) + non-exported `detail::` leaves + one
  TU-local `const UnitTest` + non-exported `const UnitTest &<node>Tests() noexcept` accessor per root-visible node;
  file-local sub-groups need no accessor; `memory`/`containers` assembled in `Core.Tests.cpp`.
  Suite roots forward-declare each accessor and call it in `_.recurse({...})` in fixed order; only `core`/`app`
  remain `extern`. Direct-root singleton leaves use a TU-local copy plus accessor
  (`const UnitTest <leaf> = detail::<leaf>;` + `const UnitTest &<leaf>Tests() noexcept { return <leaf>; }`,
  called as `<leaf>Tests()` — no additional `Named` wrapper, which would add a tree
  level and change its `core/<...>` / `app/<...>` path); see `module-architect`.
- `engine.tests.asset` (`lib/engine/tests/asset/`) — asset-pipeline suite (`EngineAssetUnitTests`,
  `asset/` root with the same extern-umbrella + `Tests()`-accessor + POST_BUILD fixture-staging
  pattern): `asset/image` (decode/format/block/hash/error), `asset/mesh` (import/convert/layout),
  `asset/staging` (separator regression), `asset/gpu` (handles/pack/caches), `asset/gate`
  (textured render gate + arbitration + editor flow). Links `engine.image + engine.mesh +
  engine.app + engine.rhi + engine.shader + engine.core + engine.math + engine.tests`
  (private GLFW for GPU integration); test dependencies never change the production graph.
- `engine.tests.sim` (`lib/engine/tests/sim/`) — renderer-independent simulation suite (`EngineSimUnitTests`,
  `sim/` root with the same extern-umbrella + `Tests()`-accessor pattern, no fixture staging):
  `sim/chunk_grid` (world geometry + coordinate/residency queries), `sim/tick` (fixed-timestep driver
  ordering/backlog), `sim/snapshot` (save/load/versioned apply round-trips), `sim/ecs`, `sim/worldgen`,
  `sim/physics`. Links PUBLIC `engine.core + engine.math + engine.sim + engine.tests`, PRIVATE `engine.physics`
  (GLFW-free, CPU-only; TIMEOUT 300).
- Shared infra in `lib/engine/tests/shared/` (`engine.tests`: `parseCli()`, `runSuite()`).
- Tests use `PPR_UNIT_TEST` macros from `lib/engine/tests/include/pP/UnitTest.h` (`UnitTest.h` always; add `Macros.h`/third-party headers to the global fragment only when the body needs them).

## Map maintenance

Keep this document focused on finding code. Update links and summaries with
their corresponding source changes; put durable rules in `AGENTS.md` instead.

- Diagrams: rendered PlantUML galleries under `docs/diagrams/` — `app-game-engine-arch/`, `engine.app/`, and `readme/`.
