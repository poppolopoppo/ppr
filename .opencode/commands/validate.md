---
description: Run the validation skill — compile the full project in every relevant build config, run engine test suites, and review local diffs against AGENTS.md.
---

Run the `validation` skill against the current working tree.

{user's optional scope: specific presets, targets, or files to focus validation on}

## What this does
1. Detect the platform and the relevant CMake presets (Windows → msvc-dev/msvc-live/msvc-rel; Unix → clang-dev/clang-rel).
2. Configure and build the FULL project in each preset using parallel background subagents (one per preset).
3. Run the engine test suites (`EngineCoreTests`, `EngineAppTests` / `ctest`) for each preset.
4. Review the local git diff against AGENTS.md conventions across 10 dimensions.
5. Produce a single pass/fail summary: build status per preset, test status per preset, and review findings by severity (Error / Warning / Suggestion).

## Orchestration notes
- The orchestrator plans and delegates; build/test execution runs in background subagents. It never compiles in the main lane.
- Reconcile all subagent results on the Background Job Board before emitting the summary.
- On failure, triage via `@fixer` (build/test fixes) and `@oracle` (architecture/design), then re-run only the affected preset.

## Sample usage
```
/validate
/validate msvc-dev msvc-rel
/validate focus on lib/engine/core
```
