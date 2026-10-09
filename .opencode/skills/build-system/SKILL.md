---
name: build-system
description: >
  PPR CMake presets, target setup, dependency registration, module workarounds,
  and sanitizer guidance.
---

# Build System

## Contract

This skill is the single build authority: build facts, target kinds, build
ownership, and the persistent build shell. It does not edit CMake files or run
builds itself; the orchestrator delegates edits to `@fixer` and execution to
the validation owner or a builder lane. Tool access and IDE-side diagnostics
are governed by the `AGENTS.md` guard; the post-change checklist lives in
`validation`.

## 0 Environment precondition — one persistent Insiders vcvars shell

- Windows requires VS 18 Insiders MSVC 14.51 for `import std` /
  `CXX_MODULE_STD` (see `lib/engine/tests/core/Core.Tests.cppm:9`).
- `CMakePresets.json:52` injects `$env{VSINSTALLDIR}...SegmentHeap.cmake`;
  `VSINSTALLDIR` must point at Insiders.
- **One long-lived shell, vcvars paid once.** Open a single `cmd` shell and
  call Insiders vcvars at start-up:

  ```bat
  call "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat"
  ```

  Keep that shell open for the whole session: every configure, build, and test
  then runs inside it, carrying `VSINSTALLDIR` (load-bearing for
  `CMakePresets.json:52`) along. Reference single-vcvars patterns:
  `.slim/tmp/configure-msvc-dev.cmd` (vcvars, then `cmake --preset msvc-dev`)
  and `.slim/tmp/build2a.bat` (vcvars, then configure, then
  `cmake --build --target ...`).
- **A shell that skips Insiders vcvars fails fast.** The MSVC pre-project
  guard rejects the configure with the required `vcvars64.bat` recovery
  (historically, before the guard, configure silently picked VS 2022
  Community 14.44 plus a bad vcpkg toolchain path and the build tree was
  unusable — see `.slim/deepwork/msvc-dev-compile-recovery.md:57-58`). Never spawn
  a fresh shell per command unless it repeats the Insiders `vcvars64.bat` call
  before touching CMake.
- Toolchain inheritance in a generic/ad-hoc shell is UNVERIFIED: JetBrains
  documentation is silent on whether terminal tools inherit the IDE toolchain.
  Policy (not documented fact): an ad-hoc terminal is never a build path and
  never where toolchain discovery happens; builds run in the persistent vcvars
  shell above.
- Observed: vcvars alone is insufficient if an inherited `PATH` or an existing
  CMake cache selects LLVM-MinGW `ld`/`ar`. MSVC-only presets pin `cl`, `link`,
  and `lib` on first configure (the configuration writer handles the preset
  values). Vcpkg detects its compiler separately and may select installed VS
  2022 even with CMake correctly pinned to VS 18; MSVC-only presets set
  `VCPKG_VISUAL_STUDIO_PATH` from the activated `VSINSTALLDIR` for initial
  configure. Ninja auto-regeneration on a separate `cmake --build` invokes
  CMake without preset environment; vcvars supplies `VSINSTALLDIR` but not
  `VCPKG_VISUAL_STUDIO_PATH`. When the override is absent, the helper derives
  the normalized vcpkg path from `VSINSTALLDIR` before vcpkg install: vcpkg
  rejected `.../18/Insiders/` while detecting `.../18/Insiders`. On the first
  fresh configure, check vcpkg's `Compiler found:` output points to VS 18.
  Verify `CMAKE_CXX_COMPILER`,
  `CMAKE_LINKER`, and `CMAKE_AR` in `out/build/<preset>/CMakeCache.txt` and
  the generated archive rule: they must use the MSVC tools, not LLVM-MinGW.
  For a contaminated *existing* `msvc-dev` tree, run
  `cmake --fresh --preset msvc-dev` in the same VS 18 Insiders vcvars shell,
  then recheck; `--fresh` is a one-time stale-cache
  recovery, not the normal configure route. Compilation errors from an active
  source refactor are separate from toolchain contamination.

## Build authority

- **Builds are cmake-direct in the persistent shell** — the strategy of
  record. Inside the one long-lived Insiders `vcvars64` shell from §0:
  configure with `cmake --preset <preset>`, build with
  `cmake --build out/build/<preset> --target <target>`, test with
  `ctest --test-dir out/build/<preset> --output-on-failure` (no build/test
  presets exist in `CMakePresets.json` — it defines `configurePresets` only,
  ending at `:240`). CLion is reserved
  for debugging and code search; it is not the build runner.
- Discover the current CLion router catalog before using IDE functionality;
  historical tool counts are not a current capability guarantee. Keep configure,
  build, and test execution on this skill's persistent-shell path.
- Run configurations may trigger implicit builds. Use only executable targets;
  do not invoke a library as a run configuration.
- Builds and tests are owned above fixer (orchestrator / builder lane),
  batched — not per-edit; the builder lane keeps the persistent shell open so
  vcvars is paid once for the whole batch.
- **Fixer must NOT build or run anything.** Fixer edits and reports; the owner
  above batches configure/build/test.

## Target kinds

| Kind | Targets | Run eligibility |
|---|---|---|
| Libraries | `engine.core`, `engine.math`, `engine.shader`, `engine.rhi`, `engine.sim`, `engine.app`, `engine.image`, `engine.mesh`, `engine.physics`, `engine.tests` | never run |
| Executables | `engine.tests.core`, `engine.tests.sim`, `engine.tests.app`, `engine.tests.asset`, `game.tests`, `app.game` | may run |

Choose build targets explicitly with `cmake --build ... --target <name>` in
that persistent shell. The table distinguishes executable run eligibility from
library buildability.

Do not invent PascalCase target aliases. `run-engine-tests` is an aggregate
build target, not a test executable.

## Current project facts

- CMake 4.3+ and C++23 are required. The project enables C++ module scanning
  and `CXX_MODULE_STD`; the experimental `import std` UUID is version-gated in
  the root `CMakeLists.txt`.
- The default generator is single-config Ninja. This avoids the documented
  CMake 4.4 multi-config synthetic-target generator-expression issue.
- Supported working presets are `msvc-dev`, `msvc-live`, `msvc-rel`,
  `msvc-profile`, `clang-cl-dev`, `clang-cl-rel`, `clang-dev`, and `clang-rel`. GCC may meet a
  C++23 language prerequisite, but the hidden `gcc-dev` and `gcc-rel` presets
  do not support this project's module builds and are not validation candidates.
- On Windows, the normal validation set is `msvc-dev`, `msvc-live`, and
  `msvc-rel` (`msvc-profile` is the instrumented Release profiling preset,
  not part of normal validation); on Linux/Darwin it is `clang-dev` and `clang-rel` unless the
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
`engine.rhi`, `engine.sim`, `engine.app`, `engine.image`, `engine.mesh`,
`engine.physics`, `engine.tests`, `engine.tests.core`,
`engine.tests.sim`, `engine.tests.app`, and `engine.tests.asset`. The
module-less game executable is `app.game`.

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

- Shared test infrastructure is the `engine.tests` static library (never run);
  runnable test executables are listed in the target-kinds table.
- The ten CTest entries are `EngineCoreUnitTests`, `EngineSimUnitTests`,
  `app_a1_death_lifecycle`, `app_a2_gates`, `app_a3_display_window`, `app_b_sim`, `app_c_fence`,
  `app_d_io`, `EngineAssetUnitTests`, and `GameConfigUnitTests`.
- `PPR_TEST_LOOPS` is a cache switch: default `1` for development;
  configure with `-DPPR_TEST_LOOPS=3` for CI stress (three same-process
  shuffled loops). When reusing that build directory for development,
  explicitly restore `-DPPR_TEST_LOOPS=1`.
- There are no CTest *test presets*. Use the executables directly or
  the approved scheduling command
  `ctest --test-dir out/build/msvc-dev -j4 --output-on-failure`.
- All six app shards and `EngineAssetUnitTests` currently set `RUN_SERIAL TRUE`;
  CTest honors that exclusion even under `-j4`.
- Run only one matrix invocation per machine at a time, with no competing
  profiling/game workload. For tests requiring exclusive scheduling within
  the matrix, prefer `RUN_SERIAL` over `RESOURCE_LOCK`; neither replaces
  machine-wide exclusivity between independent invocations.
- Never use `gcc-*` presets for module builds.

## Toolchain constraints

- Developer mode enables warnings-as-errors and the configured sanitizer/static
  analysis options. `msvc-live` is the `/ZI` Edit-and-Continue Debug preset and
  intentionally disables ASan and the compiler cache; unoptimized Debug code
  comes from `CMAKE_BUILD_TYPE=Debug`, and `PPR_RELEASE_PERF_FLAGS=OFF` there
  only gates Release-genexed flags (`cmake/compiler/MSVC.cmake:70-79`).
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
| Apply bounded CMake/preset edits | `@fixer` (edit only; no build/run) |
| Configure, build, and test | validation owner / builder lane |
| Diagnose a persistent toolchain or linker failure | `@oracle` with persistent-shell or debugger evidence |
