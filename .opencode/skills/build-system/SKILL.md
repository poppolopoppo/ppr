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
- For a local edit batch, default to the affected target/dependent closure in
  `msvc-dev` on Windows or `clang-dev` on Linux/Darwin, plus the focused tests
  selected under `validation`. Do not build every preset after each edit.
  Add `msvc-live` for Edit-and-Continue/toolchain compatibility and `msvc-rel`
  or `clang-rel` for Release/optimization compatibility when affected or requested.
  The pre-merge/release platform matrix remains `msvc-dev`, `msvc-live`,
  `msvc-rel` on Windows and `clang-dev`, `clang-rel` on Linux/Darwin.
  `msvc-profile` is instrumented Release runtime profiling, not a default gate.

## Build-performance workflow

Always-on, cheap: for already-required builds retain the exact command,
preset/targets, exit code, wall time, log path, and whether configure/regeneration
occurred. This adds no builds or ETW captures.

Activate focused measurement for explicit build-speed work or an improvement
claim; broad import/re-export/header dependency changes; module/source splitting,
target-graph, compiler/linker flag, custom-command, or scheduling changes; or
unexpected fan-out, repeated regeneration, or unexplained slowdown.

The build hot-path is edit-to-ready latency for a named edit class, target closure,
and preset, not the slowest TU.

| Change under investigation | Representative workload |
|---|---|
| Implementation-local | Warm implementation edit → affected executable/test |
| Interface/import/dependency | Interface edit → dependent closure; clean build if topology changes |
| Flags, scheduling, source organization | Clean configured-tree build plus representative incremental build |
| CMake/custom commands | Configure/regenerate and no-op behavior |
| Release code generation/LTCG | `msvc-rel`, not extrapolated Debug results |
| Edit-and-Continue | `msvc-live` |

Default to `msvc-dev`; select other presets by risk. Separate dependency
acquisition and configure from compilation. Distinguish clean outputs from cold
OS cache. Use an isolated tree for cleaning/source-mutation experiments; NEVER
clean the developer's active tree.

Retain artifacts in `out/build-perf/<case>/<baseline-or-candidate>/<run>/`:
`manifest.json`, `build.log`, `ninja.before.log`, `ninja.after.log`, `summary.md`,
and conditional `compiler.json`, `raw.etl`, or `configure.json`. The manifest
records revision/dirty identity, exact commands, working/build directories,
preset/targets/parallelism, toolchain versions, edit/reset and cache preparation,
wall time, exit codes, capture status, and artifact paths. The summary separates
measured from pending evidence and records samples/spread, executed-edge count,
top-five attributable costs, proposed cause, and verdict. Read summaries first.

Isolate the current Ninja invocation; detect log compaction and deduplicate
multi-output edges. Never sum nested compiler spans into wall time or call a long
TU the critical path without dependency evidence. `vcperf` remaps process/thread
IDs for visualization: they are not physical-concurrency evidence. The builder
owns a unique capture session and stops only that session even on build failure;
preserve build failure separately from capture/conversion failure. Unsupported
tools/permissions mean profiling unavailable, not success.

### First worked example — Packet

Measured-dev: revision `bb546b05f107d54e8163970e78e908c018681a3a`
(2026-10-09), warm single-TU mtime edit of
`lib/engine/app/frame/App.Frame.Packet.cpp`, `engine.app`, `msvc-dev`, parallelism
8. Evidence: `out/build-perf/packet/baseline/run1/summary.md` and `manifest.json`.
Readiness no-op ~4.99s (dyndep regeneration only); measured build wall 609.62s,
Packet.obj Ninja 608.09s, FrontEndPass 607.81s, BackEndPass 0.163s → 99.97%
frontend. Build and `/level2` capture exited 0; no dropped events/buffers. This
passes the plan §2 actionable workflow gate, not the full pilot ladder or §8 fix
gate. Frontend mechanism and RAM-phase attribution remain unresolved.

Pending-live: `msvc-live` trace; also pending are the interface-rebuild ladder,
failure-teardown validation, and paired uninstrumented comparisons. The existing
live repro without ASan rejects ASan as primary, but is not a measured live trace
or a speedup. The next experiment is `/showIncludes` inventory, then one
`/level3` + `/templates` capture with unchanged workload.

Non-goals: new presets, always-on ETW, GUI automation, telemetry, compiler
upgrades, module-cache enablement, or disabling checks/weakening build semantics.

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
- The twelve CTest entries are `EngineCoreUnitTests`, `EngineSimUnitTests`,
  `app_a1_death_lifecycle`, `app_a2_gates`, `app_a3_display_window`, `app_b_sim`, `app_c_fence`,
  `app_d_io`, `EngineAssetUnitTests`, `GameConfigUnitTests`,
  `WorldgenValidateFrontier`, and `WorldgenValidateHollows`.
- `PPR_TEST_LOOPS` is a cache switch: default `1` for development;
  configure with `-DPPR_TEST_LOOPS=3` for CI stress (three same-process
  shuffled loops). When reusing that build directory for development,
  explicitly restore `-DPPR_TEST_LOOPS=1`.
- There are no CTest *test presets*. Use exact leaf/group executable filters for
  the selected local scope. When the full matrix is required under `validation`,
  use the approved scheduling command
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

For an authorized module compiler-ICE investigation, retain the exact command,
working directory, response/module-map inputs, toolchain, and first diagnostic.
Keep probe outputs isolated from the production build. Compile dependent probe
units against a coherent set of primary-module and partition IFCs; mixing an
old primary IFC with new partition IFCs invalidates the comparison. A successful
isolated probe establishes only the behavior it reproduces, not the cause of an
unreproduced production failure. Follow AGENTS.md's `BLOCKED_PLAN` handoff before
changing production architecture or adopting a workaround.

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
