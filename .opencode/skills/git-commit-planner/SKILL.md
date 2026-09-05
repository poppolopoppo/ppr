---
name: git-commit-planner
description: >
  Plans ordered, atomic commits from bounded local changes. Use when the user
  wants help splitting changes, writing commit messages, or preparing commits.
---

# Git Commit Planner

## Contract

This skill plans only. It never runs `git add`, `git add -p`, `git reset`, or
`git commit`, and never code-reviews. The user retains the staging and commit
trust boundary.

`/commit` has `git diff --cached` semantics: analyze only staged hunks,
including only the staged hunks of partially staged files. Inputs are literal,
concrete file or directory selections, never Git pathspecs, globs, or magic.
Use `-AllLocal` for all tracked local changes; matching untracked files are
rejected explicitly.

Use the shared [commit-message convention](references/commit-message-convention.md).
Every planning claim must trace to a changed line.

## Sequence

### 1. Enumerate and resolve scope

Use `clion_git_status` to record branch state and changed files. First exit
without further scope work on unresolved conflicts or detached HEAD.

For `/commit`, resolve only staged hunks. Resolve each literal input to its
concrete changed paths, then literalize only those resolved paths for Git calls.
A semantic scope must resolve to a concrete path list, show that list to the
user, and contain at most 15 paths before Oracle. Renames are indivisible units
containing both source and destination paths.

Exit when the resolved input has zero applicable changes; do not invoke Oracle
or build an artifact.

### 2. Enforce the hard scope gate

Reject every selected candidate with more than 15 paths or a serialized plan
over 200KB. For an unscoped over-cap request, directory chunking is automatic:

- Do not invoke `@oracle`.
- Do not write grouping JSON or build an artifact.
- Present at most eight candidate directory scopes, each containing at most 15
  applicable files.
- If a single rename unit or directory chunk cannot fit the hard caps, require
  a narrower concrete selection before continuing.

For an explicit selection rejection, require a narrower concrete selection.
This is a gate, not a request for confirmation.

### Phase 3 batch discovery

For an over-cap request, run `Get-CommitPlanBatches.ps1` and present at most
eight deterministic directory chunks. Each candidate is bounded to 15 paths and
`estimated_patch_bytes`; the builder enforces the actual serialized UTF-8 plan
limit of 204800 bytes before publishing. The user selects one batch; only that
batch goes to Oracle, build, and review. Re-plan or discover again before the
next batch. Oracle never receives more than 15 paths.

Discovery emits schema v1 with `total_batch_count`; every candidate carries the
same total and its post-selection remainder (`total_batch_count - 1`). A selected
grouping carries `discovery_selection.total_batch_count`; the builder derives the
true remaining count. If absent, the remainder is unknown.

### 3. Obtain grouping judgment

Only after the scope gate passes, send the bounded file list to one background
`@oracle` task for ordering and grouping judgment.

File-level grouping is the default: one file belongs to one concern. True hunk
splitting is an explicit exception for at most three named shared files (for
example `Core.cppm`, `CMakeLists.txt`, or test registration). Each exception
must name its selectors and why it is needed. Do not claim automatic hunk
splitting; it is not available by default today.

Order dependency foundations before consumers, keep tests with their feature,
and keep each intermediate commit buildable. Source registration, umbrella
exports, and test registration stay with the feature they enable.

### 4. Present the plan

Present a numbered plan with files, full message blocks, and concise ordering
notes. Identify unaddressed files and why they are outside the selected scope.

### 5. Build and verify the replay plan

Write the approved grouping to `.slim/grouping.json`, then build the current
plan and gate it:

```powershell
pwsh -NoProfile -File .opencode/skills/git-commit-planner/scripts/Build-CommitPlan.ps1 -GroupingJson .slim/grouping.json
pwsh -NoProfile -File .opencode/skills/git-commit-planner/scripts/Build-CommitPlan.ps1 -VerifyOnly -PlanJson .slim/commit-plan.json
```

The builder emits ordered steps carrying exact reviewed binary patch payloads;
`add_paths` are audit metadata, never replay content. Staged-only plans capture
`git diff --cached --binary`; `all-local` plans capture `git diff --binary HEAD`.
Both support tracked paths. Staged mode ignores untracked paths; `all-local`
rejects matching untracked paths explicitly. It records HEAD, full-index, and selected-content
fingerprints, publishes atomically, and removes only its default grouping input
after successful validation.

Normal plans use only `.slim/commit-plan.json`, atomically replaced after
validation. `-Out` exists solely for isolated tests and does not create a
current plan. Do not create rerun, timestamped, porcelain, or scope-copy plan
artifacts.

After review, only the user may replay a fresh plan:

```powershell
pwsh -NoProfile -File .opencode/skills/git-commit-planner/scripts/Invoke-CommitPlan.ps1 -PlanJson .slim/commit-plan.json -DryRun
pwsh -NoProfile -File .opencode/skills/git-commit-planner/scripts/Invoke-CommitPlan.ps1 -PlanJson .slim/commit-plan.json
```

Agents never invoke `Invoke-CommitPlan.ps1`. It verifies fresh HEAD/index and
scope-local content, then applies stored payloads through a temporary Git index
so the user's index and unstaged hunks are not replayed. It requires the user
to type `COMMIT` before any mutation.

Staged-mode replay never changes the real index. User-approved all-local-mode
replay reconciles only successfully committed selected paths (including rename
sources and destinations) to the resulting HEAD; unrelated staged entries are
not reset or restored.

## Routing

| Step | Owner |
|------|-------|
| Enumerate and scope gate | Orchestrator via `clion_git_status` |
| Grouping and ordering after the gate | `@oracle` background task |
| Messages and presentation | Orchestrator |
| Build and verify | Orchestrator, `Build-CommitPlan.ps1` |
| Replay after review | User only, `Invoke-CommitPlan.ps1` |

On a re-plan, enumerate again and reapply the scope gate before requesting new
grouping judgment.
