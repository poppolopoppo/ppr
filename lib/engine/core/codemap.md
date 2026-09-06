# lib/engine/core/

## Responsibility
Umbrella foundation library (`engine.core`) re-exporting ~30 partitions into a single `pP` namespace: integer/sentinel types, hashing, utility, enums, strings, opaque type-erasure, service locator, logger, timers, unit-test framework, function wrappers, memory allocators, containers, concurrency primitives, HAL platform abstraction, and async IO. Single import point (`import engine.core;`) for engine.math/rhi/shader/app and both test executables.

## Design
- **Umbrella re-export**: `Core.cppm` lists every partition via `export import :partition;` (assert, containers ×5, concurrency ×3, enums, function ×2, hal, hashing, io ×3, logger, memory ×6, opaque, service, strings, timer, types, unit_test, utility). Downstream modules gain transitive access automatically.
- **Strong types**: `Core.Types.cppm` — `u8`–`u64`/`i8`–`i64` shorthands plus `_u8`…`_i64` literals, impostor sentinels (`default_value_v`, `zero_v`, `none_v`/`max_v`, `min_v`, `Epsilon`), `Numeric<T,TagT>` wrapper with tag-encoded contracts, `FunctionTraits`/`TFunction` helpers. `relocatable<T>` trait lives in `details` (containers partition).
- **Hashing**: `Core.Hashing.cppm` — `hash_t` with pass-through `hashValue`, rapidhash-backed `memory`/`small`/`trivial`, `mix`/`combine`, `ptr`, per-type `hashValue` (enums, integrals, floats, `Numeric`, `Uuid`, contiguous ranges), `THashable`/`DefaultHash`, range combiners (`sizedRange`/`unorderedRange`/`contiguousRange`/`anyRange`), consteval FNV-1a, `Memoizer<T>` hash cache. (The `mix`/`combine` used here are declared in `:hal`.)
- **HAL partition doubles as misc foundation**: `hal/Core.HAL.cppm` holds `simd_128_t`, `hash::mix`/`combine`, `overloaded` visitor, `Deferred`/`defer`, `randomNumberGenerator()` alongside `namespace hal` (page memory, magic ring buffer, transcoding, debugger, thread IDs/names, process, deadline timers, async IO, well-known dirs).
- **Service locator**: `Core.Service.cppm` — `IService` base keyed by compile-time `typeUid<T>()`, thread-safe `ServicesStore` with parent-chain fallback (root store + UI child store), `ServiceInjector` implicit DI. Lifetime checked by `safe_ptr<T>` (ref-counted assert in debug, raw pointer in release).
- **Opaque**: `Core.Opaque.cppm` — `opaque::Value` variant, `opaque::Block` persistent arena buffer (+ `Block::Builder`), `opaque::Unique` RAII handle, `opaque::Dict`, struct-visitor/format-context helpers.
- **Testing**: `Core.UnitTest.cppm` — `UnitTest` tree (`Context`, `Id` with `/`-joined paths, `EFlags` none/expect_fail/fork/expect_crash, fork-via-`spawnAndWait` child runs); test macros (`PPR_UNIT_TEST`, `PPR_TEST_ASSERT`) live in test-only `lib/engine/tests/include/pP/UnitTest.h`.
- **Build**: `CMakeLists.txt` registers every `.cppm` in `FILE_SET CXX_MODULES` plus per-partition `.cpp` and `${HAL_PLATFORM_SOURCES}` (11 shared HAL areas + windows-only `Random`/`RingBuffer`); links `rapidhash` privately via `setup_ppr_project`.

## Flow
- **Startup**: `game/main.cpp` imports core/math/rhi/app, constructs `Application`, which calls `hal::disableSystemErrorReporting`/`installDebugAssertHooks`, resolves dirs from `process::currentExecutablePath()`, registers services (input, window, player, RHI, shader), then runs the update/render loop with a `SharedContext` lifecycle.
- **Allocation**: callers go through `mem::GPA` → `mem::OS` (`hal::pageAlloc`) → `PagePool` bitmap → `HugePage`/`SmallPage` pools → `Arena`/`ScratchPad` (TLS), composed via `InSitu`/`Fallback`/`Threshold`/`Pooling`/`LocalCache`/`HintedPooling`, erased via `Allocator<A>`/`PMR` or adapted via `STL<A>`.
- **Messaging/events**: producers `producerReserve` → placement-new → `producerSubmit`; consumers `consumerAcquire`/`consumerRelease`; `Signal`/`select` multiplexes `IEvent`s (`RawChannel`, `IoRequest`, `DirectoryWatcher`, contexts); cancellation propagates down the `IContext` tree.
- **IO**: `IoPort::open` → `read`/`write(IoRequest&, …)` → `pollCompletions`/`waitForCompletions` drain (64-entry batches); `mapFile` for shader/asset bytes; `DirectoryWatcher::poll`/`wait` → `changes()`.
- **Diagnostics**: `PPR_ASSERT/VERIFY/ENSURE` throw or `[[assume]]` per config; `Log::Handler` drains entries on a `std::jthread`; `hal::outputDebugFmt` is debug-only.

## Integration
- **engine.math**: vector/matrix aliases and ops; `hashValue()`/`opaqueValue()` hooks from core.
- **engine.rhi / engine.shader**: RHI wraps Slang-RHI behind `IRhiService`; shader compiles via `IShaderService` reading sources through `io::mapFile` (HAL `mapFile` + `MappedFileBlob`).
- **engine.app**: owns `Application`, service stores, input/window/player/viewport/renderer layers; consumes HAL (platform selection via `PPR_HAL_PLATFORM`), concurrency, IO, timers.
- **engine.tests.core** (GLFW-free): memory, containers, concurrency, IO, strings, utility, opaque, enums, service suite wired into the core group.
- **engine.tests.app** (links GLFW): platform-dependent suites sharing `engine.tests` static lib (`parseCli`/`runSuite`).

## Key Files
- `Core.cppm` — umbrella re-export list
- `Core.Types.cppm` — integers, literals, sentinels, `Numeric`, `FunctionTraits`
- `Core.Utility.cppm` — `clamp`/`saturate`, `align*`/`divideRoundUp`, `alignof_v`, `bit_count_v`, `Expected`, `static_iota`
- `Core.Hashing.cppm` — `hash_t`, rapidhash wrappers, range hashes, FNV-1a, `Memoizer`
- `Core.Opaque.cppm` — `opaque::Value`/`Block`/`Unique`/`Dict`, builder, visitors
- `Core.Service.cppm` — `IService`, `ServicesStore`, `ServiceInjector`, `typeUid<T>()`
- `Core.Assert.cppm` — assertion machinery and failure policy
- `Core.Logger.cppm` / `Core.Timer.cppm` — async logger, `TimePoint`/`TimeSpan`, `TimerManager`
- `Core.UnitTest.cppm` — `UnitTest` framework (macros in tests include dir)
- `Core.Strings.cppm` / `Core.Enums.cppm` — lazy string transforms, enum flag utilities
- `function/` — `Callback<T>` multi-subscriber, `std23::function_ref`
- `hal/Core.HAL.cppm` — `hal` interface + shared foundation helpers
- `concurrency/` / `io/` / `memory/` / `containers/` — see subfolder codemaps
- `CMakeLists.txt` — module file sets, HAL platform sources, `rapidhash` dep
