---
name: build-system
description: >
  PPR CMake presets, target setup, dependency registration, module workarounds,
  sanitizer guidance, and build-performance diagnosis.
---

# Build System

## Contract

This skill is the single build authority: build facts, target kinds, build
ownership, and the persistent build shell. It does not edit CMake files or run
builds itself; the orchestrator delegates edits to `@fixer` and execution to
the validation owner or a builder lane. Tool access and IDE-side diagnostics
are governed by the `AGENTS.md` guard; the post-change checklist lives in
`validation`.

## 0 Environment precondition — select the host toolchain

Use the current checkout root as the working directory. On Linux select
`clang-dev` for development and `clang-rel` for Release checks; on Windows use
`msvc-dev`, adding `msvc-live` / `msvc-rel` when required. Keep separate build
trees per preset and host; never reuse Windows caches or module artifacts on Linux.

### Linux — native shell and Clang/libc++

Use a native shell with CMake 4.3+, Ninja, Clang/clang++, matching
`clang-scan-deps`, and libc++/libc++abi with `import std` module sources/manifest.
Check tool versions and paths before configuring; do not assume the system's
unversioned Clang points to the intended installation. The Clang helper discovers
`libc++.modules.json`; `PPR_LIBCXX_MODULES_JSON` can select an installed manifest.
Keep compiler, scanner, standard library and manifest compatible.

The presets obtain the vcpkg toolchain from `VCPKG_ROOT`; verify that it names an
existing installation. Linux Clang presets currently select the repository's
`x64-linux-libcxx` overlay triplet so C++ dependencies use the same standard
library ABI. Do not substitute plain `x64-linux` C++ binaries to bypass link errors.
Python 3.10+ and clang-format on PATH run the same format/style gates on Linux
and Windows; no PowerShell is required for them. Check `python --version` (or
`python3 --version`) and `clang-format --version` before a C++ edit batch.
The separate legacy build-introspection helper remains a PowerShell tool; its
availability does not control format/style validation.

From the repository root, after prerequisites are available:

```sh
cmake --preset clang-dev
cmake --build --preset clang-dev --target engine.tests.core
ctest --preset clang-dev -R '^engine[.]tests[.]core$'
```

Select targets/tests for the task under `validation`; the example covers core only.
A native Linux shell needs no vcvars activation. Verify compiler/cache paths in
`out/build/clang-dev/CMakeCache.txt`; do not infer a successful configure or build
from preset presence. Display/GPU tests require the corresponding runtime facilities;
headless core/sim checks do not establish interactive or GPU readiness.
Darwin needs separately verified toolchain/triplet settings: the current Clang
presets explicitly select a Linux triplet and are not proof of Darwin readiness.

### Windows — one persistent Insiders vcvars shell

- Windows requires VS 18 Insiders MSVC 14.51 for `import std` /
  `CXX_MODULE_STD` (see `lib/engine/tests/core/Core.Tests.cppm:9`).
- `CMakePresets.json:56` injects `$env{VSINSTALLDIR}...SegmentHeap.cmake`;
  `VSINSTALLDIR` must point at Insiders.
- **One long-lived shell, vcvars paid once.** Open a single `cmd` shell and
  call Insiders vcvars at start-up:

  ```bat
  call "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat"
  ```

  Keep that shell open for the session's configure, build, and test commands so
  it carries `VSINSTALLDIR` (load-bearing for `CMakePresets.json:56`). Use the
  commands in Build authority below; session-local scratch helpers are not
  repository prerequisites.
- For a first fresh Windows configure, shell loss, or suspected toolchain/cache
  contamination, load and follow [Windows verification and recovery](references/windows-recovery.md).
  Ordinary commands stay in this persistent activated shell, not an ad-hoc IDE terminal.

## Build authority

- **Builds are cmake-direct in the persistent shell** — the strategy of
  record. Inside the host toolchain environment from §0 (persistent Insiders
  `vcvars64` shell on Windows, native shell on Linux):
  configure with `cmake --preset <preset>`, build with
  `cmake --build out/build/<preset> --target <target>`, test with
  `ctest --test-dir out/build/<preset> --output-on-failure`. `CMakePresets.json`
  also provides build presets for supported toolchains and test presets for
  `msvc-dev`, `msvc-live`, and `clang-dev` (`cmake --build --preset <preset>`, `ctest --preset <preset>`), and
  the `run-game-<preset>` build presets invoke the `run-app-game` target. CLion
  is reserved for debugging and code search; it is not the build runner.
- Discover the current CLion router catalog before using IDE functionality;
  historical tool counts are not a current capability guarantee. Keep configure,
  build, and test execution on this skill's persistent-shell path.
- Run configurations may trigger implicit builds. Use only executable targets;
  do not invoke a library as a run configuration.
- Builds and tests are owned above fixer (orchestrator / builder lane),
  batched — not per-edit; the builder lane keeps the persistent shell open so
  Windows vcvars activation is paid once for the whole batch.
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
  `msvc-profile`, `clang-cl-dev`, `clang-cl-rel`, `clang-dev`, `clang-rel`, and
  `clang-rel-profile` (`clang-rel` codegen + `PPR_DEBUG_INFO` DWARF/frame
  pointers in its own tree, for CPU-sampling sessions). GCC may meet a
  C++23 language prerequisite, but the hidden `gcc-dev` and `gcc-rel` presets
  do not support this project's module builds and are not validation candidates.
- For a local edit batch, default to the affected target/dependent closure in
  `msvc-dev` on Windows or `clang-dev` on Linux, plus the focused tests
  selected under `validation`. Do not build every preset after each edit.
  Add `msvc-live` for Edit-and-Continue/toolchain compatibility and `msvc-rel`
  or `clang-rel` for Release/optimization compatibility when affected or requested.
  The pre-merge/release platform matrix remains `msvc-dev`, `msvc-live`,
  `msvc-rel` on Windows and `clang-dev`, `clang-rel` on Linux.
  `msvc-profile` is instrumented Release runtime profiling, not a default gate.

## Build-performance workflow

Always-on, cheap: for already-required builds retain the exact command,
preset/targets, exit code, wall time, log path, and whether configure/regeneration
occurred. This adds no builds or ETW captures.

Activate focused measurement for explicit build-speed work or an improvement
claim; broad import/re-export/header dependency changes; module/source splitting,
target-graph, compiler/linker flag, custom-command, or scheduling changes; or
unexpected fan-out, repeated regeneration, or unexplained slowdown.

When a trigger applies, load and follow [the measurement workflow](references/build-performance.md)
for representative workloads, artifact retention, capture ownership, and evidence
limits. Apply `validation`'s triggered build-performance gate to claims.

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
- Discover current CTest entries from the configured tree with
  `ctest --test-dir out/build/<preset> -N`; owning suite CMake files define
  shard filters, labels, timeouts, and resource locks. Do not rely on historical
  entry counts. `ppr_add_test_entry` (`cmake/Tests.cmake`) derives each name as
  `<target>[.<qualifier>]` and owns runtime properties.
- `PPR_TEST_LOOPS` is a cache switch: default `1` for development;
  configure with `-DPPR_TEST_LOOPS=3` for CI stress (three same-process
  shuffled loops). When reusing that build directory for development,
  explicitly restore `-DPPR_TEST_LOOPS=1`.
- Test presets mirror the dev configure presets: the preset of the same name
  runs the routine suite with `-j4` and excludes label `gpu_validation`, while
  `<preset>-gpu` runs only that label serially. Use exact leaf/group executable
  filters for the selected local scope. When the full matrix is required under
  `validation`, use the approved scheduling command
  `ctest --test-dir out/build/<preset> -j4 --output-on-failure -LE gpu_validation`,
  then a separate `-L gpu_validation` pass when required.
- Preserve the owning CMake scheduling: GPU/window entries use resource locks,
  and timing-sensitive entries may use `RUN_SERIAL`. Do not flatten these into
  a blanket serial policy.
- Run only one matrix invocation per machine at a time, with no competing
  profiling/game workload. For tests requiring exclusive scheduling within
  the matrix, honor their declared resource locks or serialization; neither replaces
  machine-wide exclusivity between independent invocations.
- Never use `gcc-*` presets for module builds.

## Toolchain constraints

- Developer mode enables warnings-as-errors and the configured sanitizer/static
  analysis options. Every `*-rel` configure preset and `msvc-live` sets
  `PPR_WARNINGS_AS_ERRORS=ON` directly (developer mode stays OFF in those
  presets), so warnings are errors in Debug and Release alike;
  `msvc-profile` inherits it through `msvc-rel`. `msvc-live` is the `/ZI` Edit-and-Continue Debug preset and
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

For an authorized module compiler-ICE investigation, retain the exact command,
working directory, response/module-map inputs, toolchain, and first diagnostic.
Keep probe outputs isolated from the production build. Compile dependent probe
units against a coherent set of primary-module and partition IFCs; mixing an
old primary IFC with new partition IFCs invalidates the comparison. A successful
isolated probe establishes only the behavior it reproduces, not the cause of an
unreproduced production failure. Follow AGENTS.md's `BLOCKED_PLAN` handoff before
changing production architecture or adopting a workaround.

Separate execution setup failures from compiler evidence. Preflight isolated
output directories, complete module-map dependency closures and rewritten
object/debug-output arguments. Preserve a failed setup attempt and correct it
within the same approved case; it does not establish that the source reproduces
or avoids an ICE. Respect an explicit invocation limit, and obtain a new design
decision before adding cases or changing production APIs.

| Work | Owner |
|---|---|
| Find existing CMake/preset patterns | `@explorer` |
| Apply bounded CMake/preset edits | `@fixer` (edit only; no build/run) |
| Configure, build, and test | validation owner / builder lane |
| Capture and measure build performance | validation owner / builder lane |
| Apply bounded performance edits | `@fixer` (edit only; no build/run) |
| Interpret build-performance evidence once | reviewer / `@oracle`; validation incorporates the verdict |
| Diagnose a persistent toolchain or linker failure | `@oracle` with persistent-shell or debugger evidence |

Benchmark execution — including workload-file touching, `vcperf` start/stop, `cmake --build`, and trace-export commands — belongs to the orchestrator/builder lane, never read-only `@explorer`; a cheaper model may be used within that authorized execution lane.
