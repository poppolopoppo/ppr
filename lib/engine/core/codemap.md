# lib/engine/core/

## Responsibility
The root `engine.core` module serves as the umbrella foundation library for the PPR game engine, re-exporting all lower-level partitions and providing the central import point for the five main engine modules (engine.math, engine.rhi, engine.shader, engine.app). It consolidates C++20 module declarations, foundational types, memory management, containers, concurrency primitives, IO abstractions, and service locator infrastructure into a single coherent namespace `pP`.

## Design
- **Module hierarchy**: Core.cppm exports `import :partition` for each sub-library, using `export import` so downstream modules gain transitive access. The umbrella re-export pattern ensures that `import engine.core` brings in all dependencies automatically.
- **Strong type safety**: Integer shorthands (`u8`–`u64`, `i8`–`i64`), sentinel values (`default_value_v`, `zero_v`, `none_v`, `umax_v`), and strongly-typed `Numeric<T,TagT>` wrappers prevent implicit conversion errors.
- **Relocatable types**: `relocatable<T>` trait (defined in Core.Containers.cppm) marks types safe for `memcpy`; used by `TagPtr`, `ArrayView`, `Stack`, and container elements. `RelPtr`/`RelativeView` are deliberately **not** relocatable (self-relative offsets must be conserved by moving).
- **Service locator**: `IService`/`ServicesStore`/`ServiceInjector` compile-time keyed via `typeUid<T>()` hash, with parent-chain fallback for hierarchical service scoping (e.g. per-viewport service stores).
- **Opaque type erasure**: `opaque::Value` (variant), `opaque::Block` (persistent byte buffer with arena allocation), `opaque::Unique` (RAII owning handle) provide type-erased data storage for serialization and reflection.
- **Memory hierarchy**: GPA (stateless wrapper over global `operator new`) → OS page allocator (`hal::pageAlloc`) → PagePool (bitmap tree) → HugePage (2 MiB) / SmallPage (32/64 KiB) pools → Arena (persistent) / ScratchPad (TLS transient) → composite allocators (InSitu, Fallback, Threshold, Pooling, LocalCache, HintedPooling).
- **Lock-free MPSC**: `RawChannel` provides a lock-free multi-producer single-consumer ring buffer for thread-safe message passing, used by concurrency and IO subsystems.
- **Compile-time event multiplexing**: `Signal<Events...>` with `select(events...)` Go-style helper enables range-for iteration over signaled events.
- **Cancellation tree**: `IContext`/`SharedContext` implements Go-style context propagation with deadline support, cancellation, and value propagation.

## Flow
- **Application entry** (`game/main.cpp`) imports `engine.core` and constructs `pP::Application`, which resolves directories, discovers registered services (input, window, player, RHI, shader), and enters the per-frame update/render loop.
- **Service registration**: Modules register services via `ServicesStore::insert<T>()`; the `ServiceInjector` retrieves them implicitly. Per-viewport child stores chain to the root with parent fallback.
- **Memory allocation**: Code requests memory through the allocator hierarchy (`mem::GPA`, `mem::OS`, `mem::HugePage`, `mem::SmallPage`, `mem::Arena`). Arena/ScopedArena supports O(1) checkpoint/restore for scope-bound allocations.
- **Concurrency**: Threads communicate via `RawChannel` (lock-free MPSC). Event signals (`PulseEvent`, `BroadcastEvent`, `Signal<Events...>`) enable wait-free notification. Contexts (`IContext`) support cancellation trees with deadline timers.
- **IO operations**: `IoPort` submits async read/write requests against caller-provided `IoRequest` events; `pollCompletions()` / `waitForCompletions()` drain completed operations. `DirectoryWatcher` monitors filesystem changes via `hal::io::openWatch`/`pollWatch`. Memory-mapped files via `MappedFile`.
- **Logging**: `Log::log()` emits entries through a writer policy; the `Log::Handler` singleton drains them on a background worker thread (`std::jthread`).

## Integration
- **engine.math**: Uses `pP::float2/3/4`, `matrix` ops, and `hashValue()` from Core.Hashing.
- **engine.rhi**: Uses `pP::rhi::*` types, projection helpers, and `IShaderService` from engine.shader; depends on Core.HAL for platform abstraction.
- **engine.shader**: Uses `IShaderService` for Slang session lifecycle and hot-reload; integrates with Core.Opaque for shader data serialization.
- **engine.app**: Constructs `pP::Application`, resolves directories, initializes services (input, window, player, RHI, shader), and drives the per-frame loop. Each viewport maintains child `ServicesStore`s chained to the root.
- **EngineCoreTests**: GLFW-free test suite for memory, containers, concurrency, IO, strings, utility, opaque, services, and enums.
- **EngineAppTests**: GLFW-linked test suite for platform-dependent tests.

## Key Files
- `Core.cppm` — umbrella module re-exporting all partitions
- `Core.Types.cppm` — integer types, sentinel values, `Numeric<T,TagT>`, `FunctionTraits`
- `Core.Utility.cppm` — math helpers (`clamp`, `saturate`, `align*`), `bit_count_v`, `Expected`, `hasFailed`, `static_iota`
- `Core.Assert.cppm` — assertion machinery (`Assertion::onFailure`, failure policy)
- `Core.Service.cppm` — `IService`, `ServicesStore`, `ServiceInjector`, compile-time `typeUid<T>()`
- `hal/Core.HAL.cppm` — HAL namespace with page memory, ring buffer, async I/O, file watching, process spawning, timers, thread IDs
- `Core.Logger.cppm` — asynchronous logger with `Log::Entry`, `Emitter`, `Handler`, severity levels
- `Core.Timer.cppm` — `TimePoint`/`TimeSpan`, `ITimerClock`, `TimerManager` with scheduling
- `Core.UnitTest.cppm` — `UnitTest` framework with `Context` (filter/fork/loop); test macros (`PPR_UNIT_TEST`) live in `lib/engine/tests/include/pP/UnitTest.h`
- `function/Core.Function.Callback.cppm` — multi-subscriber `Callback<T>` with RAII `Handle`
- `Core.Opaque.cppm` — `opaque::Value`, `opaque::Block` (+ `Block::Builder`), `opaque::Unique`, `opaqueValue()`
- `Core.Strings.cppm` / `Core.Hashing.cppm` — string utilities (lazy transforms) and rapidhash-based `hash_t` functions
- `function/Core.Function.Ref.cppm` — `std23::function_ref` (non-owning callable reference)
- `Core.Enums.cppm` — enum flag utilities (`enumOrd`, `enumCombine`, `enumContains`, bitwise operators)