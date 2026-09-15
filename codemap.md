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
player, camera, viewport, input-context, triangle pass, and UI state. The `game/`
demo hosts only a thin `TurboLarbin : ApplicationEditor` subclass with
lifecycle-hook overrides, a startup timestamp, and a debug-only ImGui demo window.

## System Entry Points

- `game/main.cpp` — Process entry point; defines `demo::TurboLarbin : ApplicationEditor` (constructed as `"ppr"` + argv span) with `initialize()` / `update(TimeSpan)` / `shutdown()` hook overrides, a startup timestamp, and a debug-only ImGui demo window; `main()` returns `app.run().value()`.
- `CMakeLists.txt` + `CMakePresets.json` — Build configuration. Public presets
  include `msvc-dev`, `msvc-live`, `msvc-rel`, `clang-cl-*`, and `clang-*`;
  `gcc-*` presets are hidden and do not support modules.
- `include/pP/Macros.h` — The single public non-module header (included via `module; #include` in the global
  fragment); engine-wide preprocessor vocabulary: build-mode + memory-poisoning detection, pointer-size
  selection, compiler-attribute portability, wide-char literals, assertions, logging, error-propagation returns,
  RAII helpers.
- `vcpkg.json` — Dependency manifest (vcpkg or CPM fallback).

## Architecture Overview

Module dependencies (flat fan-out, per CMake):

```
game/main.cpp → engine.app → engine.core / engine.math / engine.shader / engine.rhi
                 engine.rhi → engine.core / engine.math / engine.shader
                 engine.shader → engine.core
                 engine.math → engine.core
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
  `WindowViewport` supplies window geometry; `SceneView` pairs it with a
  `CameraSnapshot`. `Renderer` consumes `DrawSubmission` spans through
  `renderAndPresent` or `renderToTexture`, while passes own scene-specific draw
  resources and encoding.

## Directory Map (Aggregated)

| Directory                       | Responsibility Summary                                                                                | Detailed Map                                        |
|---------------------------------|-------------------------------------------------------------------------------------------------------|-----------------------------------------------------|
| `lib/engine/core/`              | Umbrella foundation: types, memory, containers, concurrency, IO, services, opaque, HAL.               | [View Map](lib/engine/core/codemap.md)              |
| `lib/engine/core/memory/`       | Allocator concepts, page pools, Arena/ScopedArena/ScratchPad, composite allocators, PMR, STL, poison. | [View Map](lib/engine/core/memory/codemap.md)       |
| `lib/engine/core/containers/`   | Stack/RingBuffer, Sparse/StableVector, HashMap/HashSet/FlatMap, Bitmask, views, RelPtr/TagPtr.        | [View Map](lib/engine/core/containers/codemap.md)   |
| `lib/engine/core/concurrency/`  | RawChannel (lock-free MPSC), Signal/select, IContext cancellation tree.                               | [View Map](lib/engine/core/concurrency/codemap.md)  |
| `lib/engine/core/io/`           | IoPort async I/O, MappedFile, DirectoryWatcher, IoEvent/IoResult.                                     | [View Map](lib/engine/core/io/codemap.md)           |
| `lib/engine/core/function/`     | std23::function_ref, Delegate/BroadcastCallback + Handle dispatch.                                    | [View Map](lib/engine/core/function/codemap.md)     |
| `lib/engine/core/hal/`          | HAL umbrella: page memory, ring buffer, I/O, process, timers, native transcoding.                     | [View Map](lib/engine/core/hal/codemap.md)          |
| `lib/engine/core/hal/windows/`  | Win32 HAL: VirtualAlloc2, IOCP, ReadDirectoryChangesW, CreateProcessW.                                | [View Map](lib/engine/core/hal/windows/codemap.md)  |
| `lib/engine/core/hal/linux/`    | POSIX HAL: mmap/mprotect, inotify, fork+execvp, timer_create.                                         | [View Map](lib/engine/core/hal/linux/codemap.md)    |
| `lib/engine/core/hal/darwin/`   | XNU HAL: mmap/MAP_ANON, fork+execvp, Mach sysctl debugger.                                            | [View Map](lib/engine/core/hal/darwin/codemap.md)   |
| `lib/engine/core/hal/generic/`  | Stub HAL: throw/no-op fallback for any platform.                                                      | [View Map](lib/engine/core/hal/generic/codemap.md)  |
| `lib/engine/math/`              | Single-module mango::math re-export into `namespace pP` + math:: utilities.                           | [View Map](lib/engine/math/codemap.md)              |
| `lib/engine/rhi/`               | Wraps Slang-RHI: GPU types, common projection helpers, IRhiService.                                   | [View Map](lib/engine/rhi/codemap.md)               |
| `lib/engine/shader/`            | Wraps Slang: IShaderService, SharedModule, row-major session.                                         | [View Map](lib/engine/shader/codemap.md)            |
| `lib/engine/app/`               | Application umbrella: slim Application base + ApplicationEditor subclass (IClientService), re-exports all app submodules. | [View Map](lib/engine/app/codemap.md)               |
| `lib/engine/app/input/`         | Input action/key/listener + FilteredAnalog/device message layer (10 flat files).                      | [View Map](lib/engine/app/input/codemap.md)         |
| `lib/engine/app/platform/`      | IPlatform 9-method interface + create() factory, platform errc/version helpers.                        | [View Map](lib/engine/app/platform/codemap.md)      |
| `lib/engine/app/platform/glfw/` | GLFW backend: IPlatform + IInputService + IPlayerService + IWindowService.                            | [View Map](lib/engine/app/platform/glfw/codemap.md) |
| `lib/engine/app/player/`        | IPlayerService, Player::Graph state machine.                                                          | [View Map](lib/engine/app/player/codemap.md)        |
| `lib/engine/app/renderer/`      | Content-free Renderer (surfaces, queue, submission) + TrianglePass; boundary Types header-only (.cppm, no Types.cpp). | [View Map](lib/engine/app/renderer/codemap.md)      |
| `lib/engine/app/scene/`         | Camera + controller (lookat view, view*projection snapshot).                                          | [View Map](lib/engine/app/scene/codemap.md)         |
| `lib/engine/app/service/`       | Five app service contracts: client/input/player/ui/window (behavior in platform/UI/Editor).            | [View Map](lib/engine/app/service/codemap.md)       |
| `lib/engine/app/ui/`            | UI layer (ImGui integration, IUIService; overlay shader embedded in `App.UI.ImGui.cpp`, not an asset). | [View Map](lib/engine/app/ui/codemap.md)            |
| `lib/engine/app/window/`        | IWindowService lifecycle + Viewport geometry (moved from renderer).                                   | [View Map](lib/engine/app/window/codemap.md)        |
| `cmake/`                        | Root CMake: presets, compilers, sanitizers, dependencies.                                             | [View Map](cmake/codemap.md)                        |
| `cmake/compiler/`               | Per-compiler flag config (MSVC, Clang, GCC, sanitizers).                                              | [View Map](cmake/compiler/codemap.md)               |
| `cmake/external/`               | External dependency CMake (CPM/vcpkg: SlangRHI, DearImGui, GLFW).                                     | [View Map](cmake/external/codemap.md)               |
| `game/`                         | Thin demo exe (`app.game`): `TurboLarbin : ApplicationEditor` + `main()`; POST_BUILD stages DLLs + `shaders/`. | [View Map](game/codemap.md)                         |
| `include/pP/`                   | Single public non-module header `Macros.h` (build-mode/poison detection, attributes, assertions, logging, error returns, RAII helpers). | [View Map](include/pP/codemap.md)                   |
| `assets/`                       | Shader-only runtime asset root; Slang sources compiled at startup by `engine.shader`.                 | [View Map](assets/codemap.md)                       |
| `assets/shaders/`               | Slang shader sources (including `triangle.slang`).                                                     | [View Map](assets/shaders/codemap.md)               |

> Removed locations (do not look here): `lib/engine/app/camera/` → moved to `lib/engine/app/scene/`;
> `lib/engine/app/input/device/` → consolidated into the unified `:input.device` partition; renderer `App.Viewport`
> → moved to `:window.viewport`; `App.Input.Mapping`/`App.Input.Replay` partitions deleted (mapping merged into
> `:input.action`).

## Test Infrastructure

- `engine.tests.core` (`lib/engine/tests/core/`) — GLFW-free; memory, containers, concurrency, IO, strings, opaque,
  services, enums. Thematic private groups (23 files: per-area splits such as `Core.Allocator.Tests.cpp`,
  `Core.Memory.Slab/Arena/PagePool.Tests.cpp`, `Core.Containers.*.Tests.cpp`, `Core.Concurrency.*.Tests.cpp`,
  `Core.Enums/Math/Strings/Utility.Tests.cpp`, `Core.Opaque/Service.Tests.cpp` (`14` top-level groups); umbrella exports only `extern const UnitTest core`.
- `engine.tests.app` (`lib/engine/tests/app/`) — links GLFW for platform-dependent tests. Thematic private groups
  (16 files: `App.Player/PlayerService/Player.Graph`, `App.Devices/Input.Listener/FilteredAnalog/WindowInput`,
  `App.Shader/Viewport/RenderView/PixelReadback`, `App.Camera/Quaternion`, `App.ImGuiRouting/ImguiDpi/ZeroVProbe`
  `Tests.cpp` + `App.Tests.cpp` root of 28 nodes (`11` app singletons via copy-alias); umbrella exports only `extern const UnitTest app`.
- Group pattern: `module engine.tests.<suite>;` impl unit (PRIVATE SOURCES) + non-exported `detail::` leaves + one
  `extern const UnitTest <group>` per file; sub-groups live with their parent; `memory`/`containers` assembled in `Core.Tests.cpp`.
  Singleton leaves under the root re-expose via copy (`extern const <leaf> = detail::<leaf>;`, never a new `Named`); see `module-architect`.
- Shared infra in `lib/engine/tests/shared/` (`engine.tests`: `parseCli()`, `runSuite()`).
- Tests use `PPR_UNIT_TEST` macros from `lib/engine/tests/include/pP/UnitTest.h` (`UnitTest.h` always; add `Macros.h`/third-party headers to the global fragment only when the body needs them).

## Map maintenance

Keep this document focused on finding code. Update links and summaries with
their corresponding source changes; put durable rules in `AGENTS.md` instead.

- Diagrams: rendered PlantUML galleries under `docs/diagrams/` (e.g. `docs/diagrams/engine.app/`).

