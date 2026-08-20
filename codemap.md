# Repository Atlas: PPR Game Engine

## Project Responsibility
A high-performance, real-time C++23 game engine built on C++20 modules. PPR provides a layered foundation (core → math → shader → rhi → app) with a service-locator architecture, a tiered allocator hierarchy, lock-free concurrency primitives, a platform HAL (Windows/Linux/Darwin/Generic), Slang-based shader compilation, and a Slang-RHI GPU abstraction. The `game/` demo hosts the `Application` run loop.

## System Entry Points
- `game/main.cpp` — Process entry point; defines `demo::TurboLarbin : pP::Application` and calls `app.run()`.
- `CMakeLists.txt` + `CMakePresets.json` — Build configuration (presets: `msvc-dev`, `msvc-live`, `msvc-rel`, `clang-cl-*`, `clang-*`, `gcc-*`, `developer`, `vcpkg`, `default`).
- `include/pP/Macros.h` — The single public header; engine-wide macros (assertions, logging, attributes, error handling).
- `vcpkg.json` — Dependency manifest (vcpkg or CPM fallback).

## Architecture Overview
Module dependency chain (entry → foundation):
```
game/main.cpp → engine.app → engine.rhi → engine.shader → engine.math → engine.core
                                  ↘ engine.core (HAL, memory, containers, concurrency, IO)
```
- **Service Locator**: `IService` → compile-time `typeUid<T>()` → `ServicesStore` with parent-chain fallback → `ServiceInjector`. Per-viewport child stores (`m_scene_services`, `m_ui_services`) chain to the root.
- **Allocator Composition**: `TAllocator` → `TOwningAllocator` → `TBlockAllocator` → `TArenaAllocator`; concrete allocators composed via `InSitu`, `Fallback`, `Threshold`, `Pooling`, `LocalCache`, `HintedPooling`; wrapped by `Allocator<A>`, `PMR`, `STL<A>`.
- **Concurrency**: `RawChannel` (lock-free MPSC), `Signal<Events...>` / `select()` (compile-time event multiplexing), `IContext`/`SharedContext` (Go-style cancellation tree).
- **Matrix Convention**: Row-major, row-vector `mul(float4, matrix)`; `EProjectionConvention` handles D3D vs Vulkan Z-ranges. Set at the Slang session level (`SLANG_MATRIX_LAYOUT_ROW_MAJOR`).

## Directory Map (Aggregated)

| Directory | Responsibility Summary | Detailed Map |
|-----------|------------------------|--------------|
| `lib/engine/core/` | Umbrella foundation: types, memory, containers, concurrency, IO, services, opaque, HAL. | [View Map](lib/engine/core/codemap.md) |
| `lib/engine/core/memory/` | Allocator concepts, page pools, Arena/ScopedArena/ScratchPad, composite allocators, PMR, STL, poison. | [View Map](lib/engine/core/memory/codemap.md) |
| `lib/engine/core/containers/` | Stack/RingBuffer, Sparse/StableVector, HashMap/HashSet/FlatMap, Bitmask, views, RelPtr/TagPtr. | [View Map](lib/engine/core/containers/codemap.md) |
| `lib/engine/core/concurrency/` | RawChannel (lock-free MPSC), Signal/select, IContext cancellation tree. | [View Map](lib/engine/core/concurrency/codemap.md) |
| `lib/engine/core/io/` | IoPort async I/O, MappedFile, DirectoryWatcher, IoEvent/IoResult. | [View Map](lib/engine/core/io/codemap.md) |
| `lib/engine/core/function/` | function_ref, overloaded visitor, function type aliases. | [View Map](lib/engine/core/function/codemap.md) |
| `lib/engine/core/hal/` | HAL umbrella: page memory, ring buffer, I/O, process, timers, native transcoding. | [View Map](lib/engine/core/hal/codemap.md) |
| `lib/engine/core/hal/windows/` | Win32 HAL: VirtualAlloc2, IOCP, ReadDirectoryChangesW, CreateProcessW. | [View Map](lib/engine/core/hal/windows/codemap.md) |
| `lib/engine/core/hal/linux/` | POSIX HAL: mmap/mprotect, inotify, fork+execvp, timer_create. | [View Map](lib/engine/core/hal/linux/codemap.md) |
| `lib/engine/core/hal/darwin/` | XNU HAL: mmap/MAP_ANON, fork+execvp, Mach sysctl debugger. | [View Map](lib/engine/core/hal/darwin/codemap.md) |
| `lib/engine/core/hal/generic/` | Stub HAL: throw/no-op fallback for any platform. | [View Map](lib/engine/core/hal/generic/codemap.md) |
| `lib/engine/math/` | Wraps mango::math into `namespace pP` (vectors, matrices, lookAt, hashValue). | [View Map](lib/engine/math/codemap.md) |
| `lib/engine/rhi/` | Wraps Slang-RHI: GPU types, projection conventions, IRhiService. | [View Map](lib/engine/rhi/codemap.md) |
| `lib/engine/shader/` | Wraps Slang: IShaderService, SharedModule, row-major session. | [View Map](lib/engine/shader/codemap.md) |
| `lib/engine/app/` | Application umbrella: Application lifecycle, re-exports all app submodules. | [View Map](lib/engine/app/codemap.md) |
| `lib/engine/app/camera/` | Camera abstractions. | [View Map](lib/engine/app/camera/codemap.md) |
| `lib/engine/app/input/` | IInputService, InputMapping, InputAction, modifier/trigger events. | [View Map](lib/engine/app/input/codemap.md) |
| `lib/engine/app/input/device/` | Keyboard/mouse/gamepad device state layer. | [View Map](lib/engine/app/input/device/codemap.md) |
| `lib/engine/app/platform/` | IPlatform abstraction interface. | [View Map](lib/engine/app/platform/codemap.md) |
| `lib/engine/app/platform/glfw/` | GLFW backend: IPlatform + IInputService + IPlayerService + IWindowService. | [View Map](lib/engine/app/platform/glfw/codemap.md) |
| `lib/engine/app/player/` | IPlayerService, Player::Graph state machine. | [View Map](lib/engine/app/player/codemap.md) |
| `lib/engine/app/renderer/` | Renderer: multi-viewport frame submission, ViewportEntry. | [View Map](lib/engine/app/renderer/codemap.md) |
| `lib/engine/app/service/` | App-level service registration/lifecycle. | [View Map](lib/engine/app/service/codemap.md) |
| `lib/engine/app/ui/` | UI layer (ImGui integration, IUIService). | [View Map](lib/engine/app/ui/codemap.md) |
| `lib/engine/app/window/` | IWindowService: monitor enumeration, window lifecycle. | [View Map](lib/engine/app/window/codemap.md) |
| `cmake/` | Root CMake: presets, compilers, sanitizers, dependencies. | [View Map](cmake/codemap.md) |
| `cmake/compiler/` | Per-compiler flag config (MSVC, Clang, GCC, sanitizers). | [View Map](cmake/compiler/codemap.md) |
| `cmake/external/` | External dependency CMake (CPM/vcpkg: SlangRHI, DearImGui, GLFW). | [View Map](cmake/external/codemap.md) |
| `game/` | Entry point (main.cpp) + VideoGameApp CMake target. | [View Map](game/codemap.md) |
| `include/pP/` | Public header `Macros.h` (assertions, logging, attributes). | [View Map](include/pP/codemap.md) |
| `assets/` | Runtime assets (Slang shaders). | [View Map](assets/codemap.md) |
| `assets/shaders/` | Slang shader sources (triangle.slang, hot-reloadable). | [View Map](assets/shaders/codemap.md) |

## Test Infrastructure
- `EngineCoreTests` (`lib/engine/tests/core/`) — GLFW-free; memory, containers, concurrency, IO, strings, opaque, services, enums.
- `EngineAppTests` (`lib/engine/tests/app/`) — links GLFW for platform-dependent tests.
- Shared infra in `lib/engine/tests/shared/` (`EngineTestsShared`: `parseCli()`, `runSuite()`).
- Tests use `PPR_UNIT_TEST` macros from `lib/engine/tests/include/pP/UnitTest.h` (NOT `include/pP/Macros.h`).

## Conventions
- C++20 modules: `.cppm` = interface (exports), `.cpp` = implementation; partitions `engine.core:partition`; umbrella `export import :partition;`.
- `constexpr`/`[[nodiscard]]`/`noexcept` by default; no raw loops; comments only for non-obvious code.
- Commit rule: new source + its CMakeLists.txt registration in the same commit.
