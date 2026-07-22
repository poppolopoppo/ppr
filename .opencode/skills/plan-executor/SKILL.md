---
name: plan-executor
description: >
  Executes structured refactoring plans from .opencode/plans/*.md phase by
  phase, with compile verification and test validation after each step. Use
  this skill whenever the user says "execute the plan", "run the plan",
  "apply the refactoring", "implement phase", or "commit the changes".
triggers: execute, plan, phase, refactoring, commit
---

# Plan Executor

Execute structured plans phase by phase with build-verify loops. Plans
are markdown files in `.opencode/plans/` with a specific phase/step
structure, typically produced by `git-commit-planner` or the
`/parallel-plan` command.

---

## Step 1 — Read the plan

Read the target plan file from `.opencode/plans/<name>.md`. Identify:

- **Phases** (major sections, typically `## Phase N — Title`)
- **Steps** within each phase (file operations, edits, CMake changes)
- **Dependencies** between phases (sequential ordering)

If multiple phases exist, execute them in order — each phase produces a
buildable intermediate state.

---

## Step 2 — Execute each phase

For each step in the current phase, apply the operation:

| Operation | How |
|-----------|-----|
| File rename | `git mv <old> <new>` |
| File move | `git mv <old> <new-path>` |
| Content edit | Use `edit` tool — match exact old text, replace with new |
| Import update | `sed` or manual edit — update `import :old_name` → `import :new_name` |
| CMake change | Edit `CMakeLists.txt` — add/remove source file entries |
| New file | Create with `write` tool, register in CMakeLists.txt |

After ALL steps in the phase are applied:

---

## Step 3 — Build and test

Run the build and test suite to verify the phase is correct:

Prefer using CLion run configurations when available (see `clion-tools` skill):

```
clion_execute_run_configuration(configurationName="EngineCore")
clion_execute_run_configuration(configurationName="EngineCoreTests", programArguments="--shuffle")
```

If CLion is unavailable, use CMake presets directly:

```powershell
cmake --build --preset msvc-dev --target EngineCore
cmake --build --preset msvc-dev --target EngineCoreTests
ctest --preset msvc-dev
```

Use the appropriate preset for your platform (`msvc-dev`, `clang-dev`, `gcc-dev`).

**If build or tests fail:**

1. Read the error output carefully
2. Fix the issue (missing import, wrong path, syntax error)
3. Rebuild and retest
4. Repeat until the phase passes

**If build and tests pass:**

---

## Step 4 — Commit the phase

Load the `git-commit-planner` skill to analyze the current changes and produce an
ordered commit plan with conventional commit messages. Each commit must be a
buildable intermediate state — no phase leaves the tree broken.

---

## Step 5 — Proceed to next phase

Repeat Steps 2–4 for each remaining phase. When all phases are
committed, output a summary of what was accomplished.

---

## Failure handling

If a step cannot be completed (e.g., a file doesn't exist, a rename
conflicts, a build error is unfixable):

1. **Stop** — do not proceed to the next step
2. **Report** — describe the exact failure, the file and line
3. **Offer options** — ask the user whether to:
   - Skip to the next step
   - Rollback the phase (`git checkout -- .`)
   - Fix manually and resume from the current step

---

## Constraints

- Always build after each phase, never after multiple phases
- Always test after build (`EngineCoreTests --shuffle` catches ASAN violations)
- Never modify the plan file itself unless the user asks
- Keep commits per-phase, never batch phases into one commit
- If a phase has no code changes (pure planning), skip build/test
