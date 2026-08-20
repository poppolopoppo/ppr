---
name: validation
description: >
  Post-change validation checklist for the PPR engine. Compiles the FULL
  project in every build configuration relevant to the current platform,
  runs the engine test suites, reviews local modifications against AGENTS.md,
  and gates on CLion IDE inspections (get_file_problems) — zero remaining
  errors/warnings on changed files. Use this skill whenever the user says
  "validate", "verify before commit", "run engine tests", or after any code
  change. Builds run in parallel background subagents; the code review and
  inspection gate run serially in the main flow.
---

# Validation

Run after every modification, before committing.

## Contract

This skill performs post-change validation of the PPR engine. It **does not**
edit any files itself, auto-fix issues inline, or modify the working tree.
Its output is a validated, reconciled report covering build status per preset,
test status per preset, code review findings by severity, and IDE inspection
status (errors/warnings on changed files). The orchestrator drives the skill;
configure/prepare runs sequentially in the main flow, build and test cycles
are delegated to parallel background subagents, and the main lane never
compiles or runs `ctest` itself. Remediation of any failure (build, test,
review, or inspection) happens exclusively through delegated subagents
(`@fixer` for bounded edits, `@oracle` for false-positive adjudication).
The skill is triggered by the orchestrator on commands such as "validate",
"verify before commit", "run engine tests", or after any code change.

## Subagent routing

| Step | Delegate to | Why |
|------|-------------|-----|
| Configure + build each preset | background `task` subagent per preset (general/fixer, shell access) | Heavy, parallelizable; keeps orchestrator lane free |
| Run `EngineCoreTests` / `EngineAppTests` / `ctest` | same build subagent | Co-located with the build it validates |
| IDE inspection gate (Step 1.5) | orchestrator (main flow, `clion` MCP) | Needs CLion MCP; uses `clion_git_status` + `clion_get_file_problems` |
| Triage build/test failures | `@fixer` + `@oracle` | Bounded fixes and architecture calls |
| Triage inspection problems | `@fixer` (fix) + `@oracle` (false-positive adjudication) | Same machinery as other failures |
| Diff review (11 dimensions) | `code-reviewer` skill (or `@oracle`) | Independent review lane |

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
  before the summary is presented; run the code review and inspection gate
  serially in the main flow after all subagents return.
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

## Step 1.5 — IDE inspection gate (main flow, CLion MCP)

After builds/tests return and before code review, run an IDE inspection sweep
on every changed file. This gate catches problems the compiler and tests miss
(IntelliJ inspections: unused includes, deprecated APIs, module-partition
naming, etc.).

**Procedure:**
1. Enumerate changed files. Prefer `clion_git_status(includeUntracked=true,
   projectPath="E:/Code/ppr")` for project-aware enumeration; fall back to
   shell `git status --porcelain` + `git diff --name-only HEAD` if CLion is
   unreachable. Filter to source paths under `lib/`, `game/`, `include/`,
   `cmake/`; exclude `out/`, `build/`, `_deps/`, `vcpkg_installed/`,
   `cmake-build-*` (mirror `.gitignore`).
2. Batch-call `clion_get_file_problems(filePath=<f>, errorsOnly=false,
   projectPath="E:/Code/ppr")` per file — ~8 concurrent calls per message.
   See `clion-tools` SKILL.md §6 for the exact signature.
3. Collect all problems. Errors and warnings both block the gate.
4. **Resolution loop** (max 3 rounds):
   - Dispatch `@fixer` per problem/batch to fix.
   - Re-run `clion_get_file_problems` on touched files only.
   - **False-positive path (warnings only — errors are never suppressible):**
     if `@fixer` reports a warning as a false positive, spawn `@oracle` to
     adjudicate against the actual source. If `@oracle` confirms the false
     positive, `@fixer` inserts a suppression comment (`//noinspection
     <InspectionId>` / `// NOLINT`) citing the inspection ID and rationale.
     If `@oracle` refutes, dispatch a NEW `@fixer` with explicit confirmation
     that the issue is real.
   - Suppressed warnings count as resolved but are listed in the report with
     their justification.
5. Exit: zero errors AND zero warnings (fixed or oracle-approved-suppressed)
   → gate green. Leftovers after 3 rounds → blocking ❌ entries in the report.

**Fallback:** if CLion MCP is unreachable, the gate is reported as
**RED/SKIPPED** in the aggregate report — never silently green. Builds and
tests are unaffected.

## Step 2 — Code review (serial, AGENTS.md)

After all subagents return and the inspection gate has resolved, load the
`code-reviewer` skill in the MAIN flow and review the local modifications:

```bash
git diff HEAD
git diff --cached
git log --oneline -8
```

The `code-reviewer` skill itself uses CLion MCP tools for evidence gathering
(`clion_search_symbol`, `clion_search_text`, `clion_read_file`,
`clion_get_file_problems`, etc.) — see that skill for details. Feed the
inspection-problem list from Step 1.5 into the review so `[IDE]` findings and
dimension findings reconcile.

Classify changed files by zone (`lib/`, `game/`, `*Tests*`, `cmake/`) and
review across the 11 dimensions: C++ standard usage, template complexity,
memory patterns, cache behavior, threading model, exception safety/noexcept,
compile-time/module impact, undefined behavior, design decisions, `safe_ptr`
lifetime, and function design (honesty, signature empathy, abstraction levels).
Produce a per-zone report with a severity summary (Error / Warning / Suggestion).

Never weaken assertions or tests to silence a finding — fix the root cause.

## Step 3 — Aggregate report

Combine everything into one pass/fail summary:
- Build status per preset (dev + live + rel).
- Test status per preset (list any failures).
- Review findings by severity (Error / Warning / Suggestion).
- **Inspection status**: N files checked, X fixed, Y suppressed (with
  rationale), Z open.

All green → validation complete. Any red → Step 4.

## Step 4 — Failure triage

- **Build error:** read the error, fix the source, re-run only the affected
  preset's subagent (no need to rebuild unrelated presets).
- **Test failure:** debug with CLion MCP tools in the main flow (preferred):
  - `clion_xdebug_start_debugger_session(configurationName="EngineCoreTests", projectPath="E:/Code/ppr")`
  - `clion_xdebug_set_breakpoint(filePath=..., line=..., projectPath="E:/Code/ppr")` at the failing test
  - `clion_xdebug_control_session(action="RESUME", projectPath="E:/Code/ppr")` → inspect → step.
  - Reproduce a single failure via the CLI when CLion is unavailable:
    `out/build/<preset>/EngineCoreTests --run-test <path>`.
- **Review finding:** Errors and Warnings block the change; Suggestions are
  advisory.
- **Inspection problem:** delegate fix to `@fixer` (bounded edits via
  `clion_apply_patch` / `clion_create_new_file`). Warning false-positives
  adjudicated by `@oracle` (confirmed FP → suppression comment via fixer;
  refuted → new fixer told the issue is real). Re-run `clion_get_file_problems`
  on touched files only; iterate ≤3 rounds. Leftovers → blocking ❌.

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
- **Inspection gate:** zero errors AND zero warnings on changed files (fixed
  or oracle-approved-suppressed). Errors are never suppressible. Max 3
  resolution rounds. CLion unreachable → gate reported RED/SKIPPED, never
  silently green.
