---
name: module-architect
description: >
  Authoritative rules for PPR C++20 module partitions, exported API boundaries,
  imports, umbrella re-exports, and CMake source registration.
---

# Module Architect

## Authority and scope

This is the single authority for PPR module partition naming, exported API
boundaries, imports, umbrella re-exports, and module-source registration. It
guides the orchestrator; `@fixer` performs bounded source and CMake edits.

## Module and file rules

| Kind | Module form | File form |
|---|---|---|
| Primary interface | `export module engine.core;` | `Core.cppm` |
| Partition interface | `export module engine.core:memory.arena;` | `Core.Memory.Arena.cppm` |
| Partition implementation | `module engine.core; import :memory.arena;` | `Core.Memory.Arena.cpp` |
| Core test group | `module engine.tests.core;` (impl unit, PRIVATE SOURCES) | `Core.Allocator.Tests.cpp` |
| App test group | `module engine.tests.app;` (impl unit, PRIVATE SOURCES) | `App.Player.Tests.cpp` |

- A colon separates a module from its partition; dots express partition
  hierarchy and are preserved in the filename suffix.
- `.cppm` contains the exported declaration surface and definitions necessary
  for exported templates and inline functions. Keep non-inline, non-template
  definitions in the matching `.cpp` unless explicit instantiation closes the
  supported types.
- Module source files begin with a global module fragment (`module;`) before
  textual includes. `"pP/Macros.h"` is the normal project include; wrappers may
  include required third-party headers in that fragment.
- `module engine.<library>;` is used by implementations, never
  `module engine.<library>:partition;`.

## Import policy

- Add `import std;` **only when the file's declarations or definitions use
  standard-library entities**. Do not add it mechanically, and never replace it
  with standard-library header includes in a module source.
- Within one parent module, import a partition as `import :name;`.
- Across module targets, import the public primary module (for example,
  `import engine.core;`). Do not import another target's partition.
- An implementation imports the partition it defines after reopening its parent
  primary module. Avoid importing both the full parent module and one of its
  partitions in the same file.

## Export and API boundary rules

- Export only declarations intended for consumers. Keep helpers and internal
  implementation details unexported.
- Every externally consumable partition is re-exported from its primary
  interface with `export import :partition;`; place it consistently with the
  existing umbrella ordering.
- New APIs follow the project function-design rules: honest dependencies,
  minimal parameter surface, explicit ownership/error contracts, and one
  abstraction level per body.
- A partition must not create a dependency that reverses the established module
  graph. Confirm the target dependency before adding an import or CMake link.

## CMake source registration

For each new or renamed module source, update the owning target's
`CMakeLists.txt` in the same change and commit:

```cmake
target_sources(engine.core
    PUBLIC
        FILE_SET CXX_MODULES FILES
            memory/Core.Memory.Arena.cppm
            Core.cppm
    PRIVATE
        memory/Core.Memory.Arena.cpp
)
```

- The CMake target equals the module name for engine libraries and test
  executables: `engine.core`, `engine.math`, `engine.shader`, `engine.rhi`,
  `engine.app`, `engine.tests`, `engine.tests.core`, and `engine.tests.app`.
- Put every `.cppm` in the target's public `FILE_SET CXX_MODULES`; put matching
  `.cpp` implementation units in `PRIVATE` sources. CMake does not infer either
  registration.
- Keep the umbrella interface in that file set. Register a newly public
  partition in both the umbrella and the file set.
- Platform HAL implementation files remain in `HAL_PLATFORM_SOURCES`; retain
  the existing platform-variable pattern rather than hard-coding one platform.
- New PPR targets use `setup_ppr_project()`; dependency arguments must match
  the modules actually imported. Consult `build-system` for target setup,
  presets, and toolchain workarounds.

## Partition-change procedure

For a new, moved, split, renamed, or removed partition:

1. Name one narrow responsibility and its consumers. Keep declarations in the
   `.cppm`; put every non-inline definition in the matching `.cpp`.
2. Keep helpers and storage unexported. Re-export only the consumer contract
   from the primary interface.
3. Register the interface in `PUBLIC FILE_SET CXX_MODULES` and the matching
   implementation in `PRIVATE` sources in the same change.
4. For a move, consolidation, rename, or deletion, audit both sides: remove
   obsolete implementation/interface paths, umbrella re-exports, test imports,
   and CMake entries. Confirm no stale source remains scanned or linked.
5. Send public behavior changes to `unit-test-updater` and requested execution
   evidence to the validation owner.

## Test modules

- Core tests import `engine.core`; app tests import `engine.app`; shared test
  support is `engine.tests`.
- Test suites keep ONE exported root (`core` / `app`, declared `extern` in the
  umbrella `.cppm`, defined in the suite root `.cpp`). Thematic test groups are
  private `.cpp` impl units (`module engine.tests.<suite>;`): non-exported
  `detail::` leaves plus one `extern const UnitTest <group>` built in the same
  TU. No `export module engine.tests.<suite>:<part>` partitions.
- Register the umbrella `.cppm` in the executable's `FILE_SET CXX_MODULES` and
  every group/root `.cpp` in PRIVATE SOURCES; the suite root `.cpp`
  forward-declares each node with `extern` and recurses them in fixed order.
- Singleton leaves that sit directly under the root (e.g. app's `pixel_readback`
  or lifecycle leaves) are re-exposed from their group file via copy, never
  wrapped in a new `Named` group: `extern const UnitTest <leaf> = detail::<leaf>;`.
  Wrapping one test in a `Named` would add a tree level and change its
  `core/<...>` / `app/<...>` path.
- Tests include `"pP/UnitTest.h"` for test macros and use `PPR_TEST_ASSERT`,
  not engine assertions, in test bodies.

## Change checklist

1. Select the existing library/partition boundary and dependency direction.
2. Add or update the interface and implementation using the forms above.
3. Import only required modules, including `std` only when used.
4. Re-export consumer-facing partitions from the appropriate umbrella.
5. Register every affected source in the owning CMake target and use matching
   target dependencies.
6. Add or update behavioral tests when the public behavior changes.

## Delegation

| Work | Owner |
|---|---|
| Locate an existing module/CMake pattern | `@explorer` |
| Implement bounded module, umbrella, and CMake changes | `@fixer` |
| Build and run requested checks | validation owner / build worker |
