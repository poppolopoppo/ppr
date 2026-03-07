# AGENTS.md: Developer Guide for PPR Game Engine

This document outlines structural conventions and best practices for contributing to the game engine core and game modules.

---

## Table of Contents

1. [Project Structure](#project-structure)
2. [Build System](#build-system)
3. [C++20 Modules](#c20-modules)
4. [Type Safety](#type-safety)
5. [Assertions](#assertions)
6. [Memory \& Allocators](#memory--allocators)
7. [Core Abstractions](#core-abstractions)
8. [Unit Testing](#unit-testing)
9. [Coding Standards](#coding-standards)
10. [Platform Abstraction (HAL)](#platform-abstraction-hal)

---

## Project Structure

The codebase follows a clear separation of concerns:

| Directory | Purpose |
|-----------|---------|
| `lib/` | Core reusable components: rendering pipeline, memory management, math utilities, asset loading |
| `game/` | Application entry point, game-specific logic orchestrating lib modules |

## External File Loading

**CRITICAL:** When you encounter a file reference (e.g., @rules/general.md),
use your Read tool to load it on a need-to-know basis.
They're relevant to the SPECIFIC task at hand.

Instructions:

- Do NOT preemptively load all references - use lazy loading based on actual need
- When loaded, treat content as mandatory instructions that override defaults
- Follow references recursively when needed

---

## Build System

The build system uses modern CMake. When adding new subsystems:

1. **Create directory** under `lib/` (e.g., `lib/new_subsystem/`)
2. **Add CMakeLists.txt** defining the target
3. **Integrate** via parent's `add_subdirectory()`
4. **Link** via `target_link_libraries(consumer PRIVATE new_target)`

**Rule:** Never add a library without corresponding CMake definitions.

### CMake Configuration

The root `CMakeLists.txt` provides:

| Option | Purpose |
|--------|---------|
| `PPR_ENABLE_DEVELOPER_MODE` | Enables warnings-as-errors, sanitizers, and static analyzers |
| `PPR_ENABLE_COVERAGE` | Coverage reporting for gcc/clang |
| `PPR_ENABLE_SANITIZER_*` | Individual sanitizers (address, leak, undefined, thread, memory) |
| `PPR_ENABLE_CLANG_TIDY` | Run clang-tidy on builds |
| `PPR_ENABLE_CPPCHECK` | Run cppcheck static analyzer |
| `PPR_WARNINGS_AS_ERRORS` | Treat warnings as errors |
| `PPR_ENABLE_UNITY_BUILD` | Enable unity build for faster compilation |

### Library Targets

| Target | Purpose | Dependencies |
|--------|---------|------------|
| `EngineCore` | Core modules (containers, memory, strings) | rapidhash |
| `EngineMath` | Math utilities | EngineCore, mango |
| `EngineRHI` | Rendering interface | EngineCore, slang-rhi |
| `EngineApp` | Application layer | EngineCore, EngineRHI, glfw |
| `VideoGameApp` | Final executable | All engine libraries |

### Target Setup

Use `setup_ppr_project()` to configure a target with standard settings:

- C++23 compile features
- Compiler warnings
- Engine include directories
- Sanitizers (if enabled)

### File Organization

```
cmake/
  HAL.cmake           # Platform detection (PPR_HAL_PLATFORM)
  Compilers.cmake     # Compiler-specific settings
  Sanitizers.cmake   # Sanitizer configuration
  StaticAnalyzers.cmake  # clang-tidy, cppcheck
  Dependencies.cmake  # VCPkg integration
  Cache.cmake       # Build cache settings
```

---

## C++20 Modules

We strictly use C++20 Modules for interfaces:

| Extension | Purpose |
|----------|---------|
| `.cppm` | Module interface (export declarations) |
| `.cpp` | Module implementation (definitions) |
| `.h` | Legacy headers (use sparingly) |
| `.hpp` | Pure template headers (rare) |

**Module naming:** `export module engine.core:containers;`

**Best practice:** Keep `.cppm` files minimal with only exports; put definitions in `.cpp`.

## Coding Standards

- **No raw loops:** Prefer algorithms and ranges
- **No comments:** Code should be self-documenting
- **constexpr everywhere:** Prefer compile-time evaluation
- **`[[nodiscard]]`:** Mark functions returning important values
- **`PPR_FORCE_INLINE`:** For hot-path functions
- **`noexcept`:** Mark functions that cannot throw

## Type Safety

The engine prioritizes compile-time safety:

- `checked_cast<ToT>(value)`: Safe narrowing/widening with assertion guards
- `std::bit_cast<ToT>(value)`: Type-pun without undefined behavior
- **Concepts:** Use C++20 concepts for template validation
- `std::size_t` / `std::ptrdiff_t`: For sizes and indices (not `int`)
- `std::span<T>`: For array views (not raw pointer + size)
- **Relocatable types:** Mark via `pP::details::relocatable<T>` when they support `memcpy`
- **Macros are forbidden:** The only macros allowed are in [@include/ppr/Macros.h](include/ppr/Macros.h) for assertions and portability.

## Assertions

A tiered assertion system:

| Macro | Behavior |
|------|----------|
| `PPR_ASSERT()` | Throws in debug, compiles away in release |
| `PPR_VERIFY()` | Evaluates expression in release (for side effects) |
| `PPR_ENSURE()` | Post-condition check; returns false on failure |

Enable via `PPR_ENABLE_ASSERTIONS` (enabled in debug builds).

## Platform Abstraction (HAL)

Platform-specific code lives in:

```
lib/engine/core/<platform>/Core.HAL.<platform>.cpp
```

- Platform selection via `PPR_HAL_PLATFORM` (windows, linux, darwin)
- Use `pP::hal::` namespace for OS/platform calls
- Provides: memory pages, filesystem, debugger, native strings

## Memory & Allocators

Modern C++23-compatible allocator system:

- `pP::mem::Allocator<T>`: Wrapper providing STL allocator compatibility
- `pP::mem::GPA`: General purpose allocator (backed by `operator new`)
- `pP::mem::OS`: OS virtual memory allocator (via `hal::pageAlloc`)
- `pP::mem::HugePage`: 2 MiB pooled pages for backend allocation
- `pP::mem::InSitu<N>`: Stack-allocated fixed-size buffer

## Core Abstractions

Essential types used throughout:

- `pP::Deferred`: RAII defer pattern via `PPR_DEFER { ... };`
- `pP::hash_t`: Type-safe hash values
- `pP::Stack<T,N>`, `pP::RingBuffer<T,N>`: Bounded containers
- `pP::SparseVector<T>`, `pP::StableVector<T>`: Dynamic containers
- `pP::HashMap<K,V>`, `pP::HashSet<K>`: Hash collections
- `pP::string_literal`, `pP::static_string<N>`: String literals
- `pP::basic_string_range<T>`: Lazy string transformations

## Unit Testing

Built-in test framework using `PPR_UNIT_TEST` macro:

### Defining Tests

```cpp
PPR_UNIT_TEST(my_test) {
    PPR_ASSERT(condition);
};
```

Use `PPR_ASSERT` inside tests. Failed assertions are caught and reported as test failures.

Tests are organized hierarchically using the `/` operator:

```cpp
PPR_UNIT_TEST(container) {
    _.recurse(Strings::char_helpers);
    _.recurse(Strings::escape_functions);
};
```

Tests are nested in `pP::tests` namespace. Parent tests aggregate child results:

```cpp
// From Core.cppm
export namespace pP::tests {
    PPR_UNIT_TEST(core) {
        _.recurse(memory);
        _.recurse(strings);
        _.recurse(containers);
    };
}
```

### Running tests

Run all core tests:
```cpp
pP::UnitTest::run(pP::tests::core);
```
