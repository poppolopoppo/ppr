# AGENTS.md: Developer Guide for PPR Game Engine

## Available Skills
Load these on demand via the `skill` tool when your task matches their domain:

| Skill | Trigger Keywords | Coverage |
|-------|-----------------|----------|
| `clion-tools` | search, find, debug, breakpoint, build, run, diagnose | CLion MCP tools for code search, debugging, building, and diagnostics — use INSTEAD of grep/glob/bash |
| `memory-allocator` | allocation, arena, pool, slab, poison | Allocator selection, composition, arena patterns, poison API, STL adapter, safe_ptr |
| `module-architect` | new module, partition, `.cppm`, test module | Module naming, file structure, umbrella registration, CMake registration, pitfalls |
| `build-system` | cmake, preset, target, dependency, linker error | Presets, setup_ppr_project, deps (CPM/vcpkg), MSVC workarounds, sanitizers |
| `hal-developer` | hal, platform, porting, syscall | 10 areas across 4 platforms, syscall mapping, stub conventions, adding a platform |
| `concurrency-patterns` | channel, signal, event, context, cancellation | RawChannel MPSC, IEvent/Signal, IContext tree, thread safety, HAL I/O integration |

## External File Loading
When you encounter a file reference (e.g., @rules/general.md), load it on demand.
Do NOT preemptively load all references. Treat loaded content as mandatory instructions.

## Build System
- Load the `clion-tools` skill when starting any task. Use CLion MCP tools INSTEAD of grep/glob/bash for code search, building, and debugging.
- CMake 4.2+, C++23, modules enabled, experimental `import std`.
- Presets: `msvc-dev` (recommended), `msvc-rel`, `clang-cl-dev`, `clang-cl-rel`, `clang-dev`, `clang-rel`, `gcc-dev`/`gcc-rel` (hidden, no modules).
- Use `setup_ppr_project(Target INTERNAL_PUBLIC_DEPS ... EXTERNAL_SYSTEM_PRIVATE_DEPS ...)` for every target (see cmake/Compilers.cmake).
- Commit rule: new source file + its CMakeLists.txt registration go in the same commit.
- Two separate test executables: `EngineCoreTests` (core, GLFW-free) and `EngineAppTests` (links glfw). Aggregate target `run-engine-tests` runs both. Build via `cmake --build out/build/msvc-dev --target EngineCoreTests` (or `EngineAppTests`). Run via `run-engine-tests` run configuration in CLion.
- Shared test infrastructure in `lib/engine/tests/shared/` (static lib `EngineTestsShared`) provides `parseCli()` and `runSuite()` to avoid duplication between test executables.

## C++20 Modules
- `.cppm` = interface (exports), `.cpp` = implementation (definitions), `.h` = Macros.h only.
- Libraries: `engine.core`, `engine.math`, `engine.rhi`, `engine.app`.
- Partitions: `engine.core:containers.hash_map` (dots = hierarchy).
- Tests: `engine.tests.core:memory` (dots for module name, colon for partition).
- File naming: `Core.<Partition>.cppm` / `Core.<Partition>.cpp`, platform HAL: `Core.HAL.<platform>.<Area>.cpp`.
- Interface pattern:
  ```cpp
  module;
  #include "pP/Macros.h"
  export module engine.core:your_partition;
  import std;
  export namespace pP { ... }
  ```
- Implementation pattern:
  ```cpp
  module;
  #include "pP/Macros.h"
  module engine.core;
  import :your_partition;
  import std;
  namespace pP { ... }
  ```
- Umbrella re-exports: add `export import :partition;` to `Core.cppm`.
- Keep `.cppm` files minimal (exports only); put definitions in `.cpp`.

## Coding Standards
- No raw loops (prefer algorithms/ranges). Comments should be exceptional — only add them for genuinely surprising or non-obvious code that cannot be clarified through naming or structure alone.
- `constexpr` everywhere, `[[nodiscard]]` on important returns, `noexcept` where possible.
- Inlining: `PPR_FORCE_INLINE` (hot paths), `PPR_NO_INLINE` (prevent), `PPR_FLATTEN` (recursive).
- Attributes: `PPR_EMPTY_BASES` (MSVC stateless wrappers), `PPR_LIFETIME_BOUND` (reference lifetime deps).
- Allowed macros: only those in `include/pP/Macros.h` — assertions (PPR_ASSERT/VERIFY/ENSURE/ASSUME), PPR_DEFER, inlining control, logging (PPR_LOG), PPR_UNIT_TEST. No others.

## Type Safety
- `checked_cast<ToT>(v)` — safe narrowing/widening + downcast (dynamic_cast debug, static_cast release).
- `safe_narrowing<IntT>` — tag type asserting round-trip on implicit conversion.
- Integer shorthands: `u8/u16/u32/u64/i8/i16/i32/i64` (Core.Types.cppm).
- Sentinel values: `default_value_v`, `zero_v`, `none_v`, `umax_v`.
- `Numeric<T, TagT>` — strongly-typed numeric wrapper.
- `hash_t` — type-safe hash value (struct with m_value, comparison, hashValue).
- `pP::details::relocatable<T>` — mark types supporting memcpy.

## Assertions
| Macro | Debug Behavior | Release Behavior |
|-------|---------------|------------------|
| `PPR_ASSERT(expr)` | Calls onFailure, throws `std::logic_error` | `PPR_ASSUME(expr)` |
| `PPR_VERIFY(expr)` | Calls onFailure, throws `std::logic_error` | Evaluates expr + `PPR_ASSUME` |
| `PPR_ENSURE(expr)` | Returns false on failure | `PPR_ASSUME(expr)` then eval |
| `PPR_ASSUME(expr)` | `[[assume(expr)]]` / `__built_assume` | Same |
- `PPR_ENABLE_ASSERTIONS` = `PPR_ENABLE_DEBUG` (`_DEBUG` or `!NDEBUG`).
- `Assertion::setFailurePolicy()` lets tests intercept assertions.

## HAL
Platform code in `lib/engine/core/<platform>/`. Supported: windows, linux, darwin, generic (stub). Selected via `PPR_HAL_PLATFORM` (cmake/HAL.cmake).
- `pP::hal`: pageAlloc/Free/Commit/Decommit/Protect/OfferToOS/ReclaimFromOS, ringBufferAlloc/Free, outputDebug, isDebuggerPresent, breakpoint.
- Sub-namespaces: `process` (executablePath, spawnAndWait, terminate), `timer` (setDeadline, cancelDeadline), `io` (async I/O, file watches), `native` (string transcoding).
- See `Core.HAL.cppm` for full API surface.

## Memory & Allocators
- Concepts: `TAllocator`, `TOwningAllocator`, `TResizableAllocator`, `TBlockAllocator`, `TArenaAllocator` (in Core.Memory.Allocator.cppm).
- Hierarchy: GPA (operator new) → OS (pageAlloc) → PagePool/BitmapTree → HugePage (2 MiB) / SmallPage (32/64 KiB) → Arena (persistent) / ScratchPad (TLS transient).
- Slab/Arena: Slab, InSituSlab, Arena<AllocatorT = HugePage>, ScopedArena (RAII watermark), ScratchPad (TLS Arena<SmallPage>).
- Composite: InSitu, Fallback, Threshold, Pooling, LocalCache, HintedPooling, Static, Allocation<T>, Allocator<>, AllocatorTraits, STL<>, Inplace.
- Poison: `poisonReserved`, `unpoisonUninitialized`, `poisonDestroyed`, `annotateContiguousContainer` (+ typed overloads). Uses `__asan_*` when ASAN enabled, debug patterns (0xAA/0xCC/0xDD) otherwise, no-op in release.
- For MSVC with `msvc-dev` preset, ASAN is auto-enabled via PPR_ENABLE_DEVELOPER_MODE.
- See `Core.Memory.*.cppm` for full type catalog.

## Core Abstractions
All types in `namespace pP`. See corresponding `.cppm` files:
- **Containers:** Stack<T,N>, RingBuffer<T,N> (bounded, trivial T); SparseVector<T>, StableVector<T>, HashMap<K,V>/HashSet<K>, FlatMap<K,V>, Bitmask<T,N>, SetBitsRange.
- **Pointers/views:** RelPtr (relative offset), TagPtr (flagged), ArrayView, RelativeView (half-size), safe_ptr, IndexIterator.
- **Strings:** string_literal, static_string<N>, char helpers (toLower, etc.), lazy transforms (caseFold, stringEscape, trim, etc.).
- **Opaque values:** opaque::Value (variant), opaque::Block (persistent), opaque::Unique (RAII owning), Block::Builder.
- **Concurrency:** IEvent/ISignal/Signal<...>, RawChannel (lock-free MPSC), IContext/SharedContext (Go-style cancellation).
- **Other:** IService/typeUid<T>, Log::Category/ELevel/Emitter, TimerManager, overloaded (visitor), std23::function_ref, sort::shellSort.

### safe_ptr<T>

- **Debug mode** (`PPR_ENABLE_DEBUG`): reference-counted lifetime checker — asserts that no `safe_ptr` outlives the pointed-to object
- **Release mode**: zero-overhead raw pointer (identical to `T*`)
- **NOT** a shared ownership pointer — the user guarantees init/destroy ordering
- All `safe_ptr` copies must be released (set to `nullptr` or go out of scope) before the owning object is destroyed
- `safe_ptr` from `unique_ptr::get()` is correct by design: the user guarantees the `unique_ptr` outlives all `safe_ptr` instances; `safe_ptr` will assert if violated
- `setDebugLifetimeCheckEnabled(bool)` controls the checker; disabled by default for services

## Unit Testing
- Define: `PPR_UNIT_TEST(name) { PPR_ASSERT(cond); };` (tests are `inline constexpr` variables).
- Flags: `UnitTest::expect_fail` (must throw), `UnitTest::fork` (child process), `UnitTest::expect_crash` (fork + expect_fail).
- Group: `_.recurse({TestA, TestB, ...})` — supports conditional inclusion via `if constexpr (PPR_ENABLE_DEBUG)`.
- Module pattern: `export module engine.tests.core:memory;` with `export namespace pP::tests { ... }`.
- CLI: `EngineTests [--run-test <path>] [--shuffle [<seed>]] [--no-shuffle] [--loop <N>] [--child-run] [--help]`.
- Fork tests spawn child process via `hal::process::spawnAndWait`. Assertions intercepted by test framework (converted to failures, not terminations).
- Run programmatically: `pP::UnitTest::run(context, pP::tests::core);`.
- See `lib/engine/tests/` for existing examples.

## Debugging with CLion
- ALWAYS use CLion xdebug MCP tools for debugging. Never use printf/logging when the debugger is available.
- Workflow: start session → set breakpoint → resume → wait for pause → inspect stack/variables → step or continue.
- Load the `clion-tools` skill for the full tool reference and examples.
- Key tools: `clion_xdebug_start_debugger_session`, `clion_xdebug_set_breakpoint`, `clion_xdebug_control_session`, `clion_xdebug_get_stack`, `clion_xdebug_get_frame_values`, `clion_xdebug_evaluate_expression`.

## Recommendations
- Use Task agents for multi-file exploration (they get fresh context)
- Batch parallel tool calls when possible
- Keep file reads targeted (use offset/limit for large files)
- When hitting blockers like compiler ICE or hard-crash, do not jump to ambitious refactors of the prepared plan: instead you **must** notify the user and ask for validation and to decide of the best direction.