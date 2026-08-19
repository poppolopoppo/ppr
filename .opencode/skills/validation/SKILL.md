---
name: validation
description: >
  Post-change validation checklist for the PPR engine. Compiles the FULL
  project in every build configuration relevant to the current platform,
  runs the engine test suites, and reviews local modifications against
  AGENTS.md. Use this skill whenever the user says "validate", "verify
  before commit", "run engine tests", or after any code change. Builds
  run in parallel background subagents; the code review runs serially in
  the main flow.
---

# Validation

Run after every modification, before committing.

## Contract

This skill performs post-change validation of the PPR engine. It **does not**
edit any files, auto-fix issues, or modify the working tree. Its output is a
validated, reconciled report covering build status per preset, test status per
preset, code review findings by severity, and failure triage. The orchestrator
drives the skill; configure/prepare runs sequentially in the main flow, build
and test cycles are delegated to parallel background subagents, and the main
lane never compiles or runs `ctest` itself. The skill is triggered by the
orchestrator on commands such as "validate", "verify before commit", "run
engine tests", or after any code change.

## Subagent routing

| Step | Delegate to | Why |
|------|-------------|-----|
| Configure + build each preset | background `task` subagent per preset (general/fixer, shell access) | Heavy, parallelizable; keeps orchestrator lane free |
| Run `EngineCoreTests` / `EngineAppTests` / `ctest` | same build subagent | Co-located with the build it validates |
| Triage build/test failures | `@fixer` + `@oracle` | Bounded fixes and architecture calls |
| Diff review (10 dimensions) | `code-reviewer` skill (or `@oracle`) | Independent review lane |

## OMO feature wiring

- **Per-agent `skills`/`mcps` allow-lists** — orchestrator has `skills: ["*"]`
  and `mcps: ["*","!context7"]` (has `clion`); build subagents need only
  shell/`cmake` access — they must NOT rely on CLion MCP tools (may be
  unavailable); `@oracle` needs `skills: ["simplify", "code-reviewer"]`
  (explicit grant in `~/.config/opencode/oh-my-opencode-slim.json` or
  project-local override — `code-reviewer` is not in `oracle`'s default
  allow-list).
- **Background orchestration** — launch one build subagent per preset in
  parallel (same message), reconcile all results on the Background Job Board
  before the summary is presented; run the code review serially in the main
  flow after all subagents return.
- **Session reuse** — reuse one build subagent session across presets to
  amortize CMake cache warm-up (invalidate when the preset set or target area
  changes); reuse one read-only session for diff retrieval in Step 2.
- **`orchestratorPrompt` routing** — trigger on "validate", "verify before
  commit", "run engine tests", or after any code change.

## Step 0 — Detect platform & presets

Run in pwsh (Windows) / bash (Unix):
- Windows (`$IsWindows` is true, or `$env:OS` is `Windows_NT`)
  → presets: **`msvc-dev`**, **`msvc-live`**, **`msvc-rel`**
- Unix (`uname -s` is `Linux` or `Darwin`) → presets: **`clang-dev`**, **`clang-rel`**

Skip `clang-cl-*` (no C++20 module support in this repo) and `gcc-*`
(hidden, no module support). Each preset builds into its own
`out/build/<preset>` dir → fully independent.

## Step 0.5 — Sequential prepare (avoid cache races)

Configure every preset ONCE, sequentially, in the main flow (never inside
parallel subagents):

```powershell
cmake --preset msvc-dev
cmake --preset msvc-live
cmake --preset msvc-rel
```

(Same pattern with `clang-dev`/`clang-rel` on Unix.)

This populates the shared `out/cpm_cache` and the per-preset
`vcpkg_installed` safely. Skip any preset whose `out/build/<preset>` is
already configured.

## Step 1 — Parallel compile + test (subagents)

Launch ONE general-purpose subagent (with shell access, not `explore`) per
preset, all in the same message so they run in parallel. Each subagent gets a
self-contained prompt and MUST return this exact template:

```
PRESET: <name>
BUILD_EXIT: <0|code>
TEST_EXIT: <0|code>
FAILED: <test names or "none">
ERRORS: <build/test errors or "none">
```

Per-subagent instructions:
1. Run from the repository root; `VCPKG_ROOT` is inherited from the environment
   (auto-detected by `cmake/compiler/MSVC.cmake`).
2. Build the FULL project (no target filter → all targets, including
   `VideoGameApp` and `run-engine-tests`):
   ```powershell
   cmake --build out/build/<preset>
   ```
3. Run the suites directly, randomized:
   ```powershell
   out/build/<preset>/EngineCoreTests --shuffle
   out/build/<preset>/EngineAppTests  --shuffle
   ```
   Fallback (no `testPresets` exist in this repo, so `ctest --preset` must NOT
   be used):
   ```powershell
   ctest --test-dir out/build/<preset> --output-on-failure
   ```
4. Collect exit codes, failed test names, and error excerpts; return the
   template above.

Subagents must NOT rely on CLion MCP tools (they may be unavailable). Use
`cmake`/`ctest` directly. Debugging is done in Step 4, in the main flow.

Aggregate all subagent results before proceeding.

## Step 2 — Code review (serial, AGENTS.md)

After all subagents return, load the `code-reviewer` skill in the MAIN flow
and review the local modifications:

```bash
git diff HEAD
git diff --cached
git log --oneline -8
```

Classify changed files by zone (`lib/`, `game/`, `*Tests*`, `cmake/`) and
review across the 10 dimensions: C++ standard usage, template complexity,
memory patterns, cache behavior, threading model, exception safety/noexcept,
compile-time/module impact, undefined behavior, design decisions, and
`safe_ptr` lifetime. Produce a per-zone report with a severity summary
(Error / Warning / Suggestion).

Never weaken assertions or tests to silence a finding — fix the root cause.

## Step 3 — Aggregate report

Combine everything into one pass/fail summary:
- Build status per preset (dev + live + rel).
- Test status per preset (list any failures).
- Review findings by severity (Error / Warning / Suggestion).

All green → validation complete. Any red → Step 4.

## Step 4 — Failure triage

- **Build error:** read the error, fix the source, re-run only the affected
  preset's subagent (no need to rebuild unrelated presets).
- **Test failure:** debug with CLion MCP tools in the main flow (preferred):
  - `clion_xdebug_start_debugger_session(configurationName="EngineCoreTests")`
  - `clion_xdebug_set_breakpoint(filePath=..., line=...)` at the failing test
  - `clion_xdebug_control_session(action="RESUME")` → inspect → step.
  - Reproduce a single failure via the CLI when CLion is unavailable:
    `out/build/<preset>/EngineCoreTests --run-test <path>`.
- **Review finding:** Errors and Warnings block the change; Suggestions are
  advisory.

## Constraints

- Presets are selected by platform; never compile `clang-cl-*` or `gcc-*` here.
- `-dev` presets run with ASAN + warnings-as-errors (Developer Mode); `-rel`
  catches optimization-only issues; `msvc-live` (Debug, `/ZI` Edit&Continue,
  no ASAN, no optimizations, no ccache) validates the Edit&Continue config.
  All must pass.
- `msvc-live` is a full uncached rebuild per cycle (no ccache; `/ZI` Debug
  codegen differs from `msvc-dev`), so budget for a full-build wait — run it
  in the parallel build batch, never serially after the other presets.
- Build the FULL project per preset, including `VideoGameApp` — tests alone do
  not prove the whole configuration compiles.
- `ctest --preset` is invalid here (no `testPresets` in CMakePresets.json);
  run executables directly or use `ctest --test-dir`.
- Subagents use `cmake`/`ctest` only; debugging uses CLion (see `clion-tools`).
- Code review findings must cite a specific AGENTS.md rule or named C++
  best practice.
- The main flow never compiles or runs `ctest` itself — build/test cycles
  always run in the background subagents.
