---
name: build-system
description: >
  PPR CMake presets, target setup, dependency registration, module workarounds,
  and sanitizer guidance.
---

# Build System

## Contract

This skill is build-system reference material. It does not edit CMake files or
run builds itself; the orchestrator delegates edits to `@fixer` and execution to
the validation owner or a build worker.

## Current project facts

- CMake 4.3+ and C++23 are required. The project enables C++ module scanning
  and `CXX_MODULE_STD`; the experimental `import std` UUID is version-gated in
  the root `CMakeLists.txt`.
- The default generator is single-config Ninja. This avoids the documented
  CMake 4.4 multi-config synthetic-target generator-expression issue.
- Supported working presets are `msvc-dev`, `msvc-live`, `msvc-rel`,
  `clang-cl-dev`, `clang-cl-rel`, `clang-dev`, and `clang-rel`. GCC may meet a
  C++23 language prerequisite, but the hidden `gcc-dev` and `gcc-rel` presets
  do not support this project's module builds and are not validation candidates.
- On Windows, the normal validation set is `msvc-dev`, `msvc-live`, and
  `msvc-rel`; on Linux/Darwin it is `clang-dev` and `clang-rel` unless the
  requested scope specifies otherwise.

## Target setup

Every PPR target uses `setup_ppr_project()` from `cmake/Compilers.cmake`:

```cmake
setup_ppr_project(engine.example
    INTERNAL_PUBLIC_DEPS engine.core
    EXTERNAL_SYSTEM_PRIVATE_DEPS some_external_target
)
```

The function enables the standard-library module support and C++23, applies
project warnings and includes, configures sanitizers/cache handling, and links
declared internal and external dependencies. Its supported dependency groups
are `INTERNAL_PUBLIC_DEPS`, `EXTERNAL_SYSTEM_PRIVATE_DEPS`, and
`EXTERNAL_SYSTEM_PUBLIC_DEPS`.

Internal module targets are `engine.core`, `engine.math`, `engine.shader`,
`engine.rhi`, `engine.app`, `engine.tests`, `engine.tests.core`, and
`engine.tests.app`. The module-less game executable is `app.game`. Do not
invent PascalCase target aliases.

## Source and dependency changes

- Follow `module-architect` for `FILE_SET CXX_MODULES`, umbrella re-exports,
  source-file registration, and import-to-dependency alignment.
- Add source registration with the source in the same commit.
- Use only real CMake targets. Resolve an external target from its defining
  CMake file before linking it.
- Do not hand-roll target compiler/module configuration that
  `setup_ppr_project()` already provides.

### Registration/removal audit

When a source is added, moved, renamed, split, or deleted, identify its owning
target first. Add each new `.cppm` to the public module file set and its matching
`.cpp` to private sources. On removal or consolidation, delete every obsolete
source registration and only the imports/re-exports made stale by that change;
audit platform lists such as `HAL_PLATFORM_SOURCES` separately. Keep the change
minimal, then hand module-surface decisions to `module-architect` and requested
configure/build evidence to the validation owner.

## Test execution facts

- Test executables are `engine.tests.core` and `engine.tests.app`; shared test
  infrastructure is the `engine.tests` static library.
- CTest tests are named `EngineCoreUnitTests` and `EngineAppUnitTests`.
- There are no CTest *test presets*. Use the executables directly or
  `ctest --test-dir out/build/<preset> --output-on-failure`.
- `run-engine-tests` is an aggregate build target, not a test executable.

## Toolchain constraints

- Developer mode enables warnings-as-errors and the configured sanitizer/static
  analysis options. `msvc-live` is the `/ZI` Edit-and-Continue Debug preset and
  intentionally disables ASan, optimization, and compiler cache.
- Do not enable a compiler cache for module targets; retain the project helper
  that disables it.
- On MSVC ASan builds, retain the STL annotation-disable workaround to avoid
  module/STL link mismatches.
- `clang-cl-*` is available for targeted work but is not part of the standard
  validation matrix unless explicitly requested.

## Delegation

| Work | Owner |
|---|---|
| Find existing CMake/preset patterns | `@explorer` |
| Apply bounded CMake/preset edits | `@fixer` |
| Configure, build, and test | validation owner / build worker |
| Diagnose a persistent toolchain or linker failure | `@oracle` with CLion evidence |
