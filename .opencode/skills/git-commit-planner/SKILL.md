---
name: git-commit-planner
description: >
  Plans atomic commits, fast-path by default, full verified replay only
  over-cap. Use when the user wants help splitting changes, writing commit
  messages, or preparing commits.
---

# Git Commit Planner

## Contract

Planning (Tiers 0-2) is non-mutating. Only after a single `yes` does
execution commit one-by-one in logical order. `/commit` has
`git diff --cached` semantics: staged hunks only, including staged hunks
of partially staged files. Inputs are literal paths only, never
pathspecs, globs, or magic.

Hygiene gates: exit when zero paths are staged. Stop on merge conflicts.
Stop on the first hook, conflict, or command error; abort the sequence
and report remaining uncommitted work. Never push, amend, or force-push;
refuse `--no-verify`, `--amend`, and any force-push flag. Never commit
`out/`, `_deps/`, `vcpkg_installed/`, `build/`, `cmake-build-*/`,
`imgui_module_bindings/`, or `.slim/` artifacts.

Use the shared [commit-message convention](references/commit-message-convention.md)
as the single canonical source. Do not duplicate or reinterpret that
convention here. Do not apply Conventional Commit `type(scope):` rules.

`@oracle` is for grouping judgment only; never code review. Route review
requests to `code-reviewer` instead.

Analysis budget: `hunk skim` only. A hunk skim is `clion_git_status` +
staged diff stat + a skim of staged hunks sufficient to group concerns.
No deep per-file review, no per-line audit. Time-box grouping.
Mechanical commits get structural check only.

Atomicity heuristic: one concern per commit. A subject containing "and"
is a split signal, unless code plus its test or CMake registration forms
one atomic feature. Every claim traces to a changed line via hunk skim
(not per-line audit). Never refuse due to size; never require narrower
selection.

Commit-path semantics: use bare `git commit` (staged index as-is), or
`git commit -- <verified fully-staged literal paths>` only after
confirming each listed path has no unstaged delta (`git diff -- <paths>`
is empty). Never pass partially-staged files by path.

The commit-plan artifact `.slim/commit-plan.json` is separate from
`.slim/push-plan.json`. This skill never writes, reads, or replays a
push-plan payload; `git-push-planner` owns that artifact and never
replays a commit-plan payload.

## Tier 0 — fast path (<=10 paths heuristic, single concern)

1. `clion_git_status` + staged diff stat + hunk skim; apply hygiene
   gates (exit on zero staged, stop on conflicts).
2. Orchestrator inline secret scan of the staged diff before drafting;
   on an actual credential hit stop and require removal and rotation
   before any draft:
   ```powershell
   git diff --cached -p | rg -i '(api[_-]?key|secret|password|token|credential|bearer|private[_-]?key)' -C 1
   ```
3. Orchestrator inline heuristic split (no `@oracle`, no `@explorer`).
4. Draft message per the canonical convention; present files + message.
5. Single `yes`, then `git commit` per commit-path semantics above.

No `@oracle`, no `@explorer`, no `.slim/*.json`, no `scripts/` in this
tier.

## Tier 1 — multi-commit (delta on Tier 0)

Tier 0 procedure, plus an in-memory ordered multi-commit plan (paths +
messages) for push-planner reuse. Triggers on EITHER heuristic
condition: <=10 paths with 2+ concerns, or 11-15 paths with any concern
count. `@oracle` opt-in only when grouping is genuinely ambiguous, for
grouping judgment only. Tier 1 never writes `.slim/commit-plan.json`.

## Tier 2 — over-cap (over 15 paths or 204800 bytes `git diff --cached` size heuristic)

1. Batch discovery: run `scripts/Get-CommitPlanBatches.ps1`, present at
   most 8 chunks. If too fragmented (>8 chunks, single-file splinters,
   or chunks violating the atomicity heuristic), the orchestrator merges
   small batches into bigger logical commits inline via
   `clion_git_status` + staged diff stat + hunk skim. Never delegate
   grouping to `@explorer`. `@oracle` off by default; opt-in only for
   genuinely ambiguous grouping judgment.
2. Order along the PPR layer order (`engine.core` -> `engine.math` /
   `engine.shader` -> `engine.rhi` -> `engine.app` -> `app.game`;
   see `AGENTS.md`). Refactor before feature. Mechanical bulk (renames,
   formatting, codemods, generated) in its own commit. Tests travel with
   the code they validate. Setup is folded into the task that needs it.
   Path-count and byte thresholds above are heuristics, not hard gates.
3. Present the ordered multi-commit sequence; a single `yes` confirms
   the whole sequence, then commit one-by-one in logical order per
   commit-path semantics. Stop on first failure (hook, conflict,
   error): abort the sequence, report remaining uncommitted.
4. Verified replay only via `scripts/Invoke-CommitPlan.ps1` on explicit
   user request, never silently. `scripts/Build-CommitPlan.ps1` builds
   the `.slim/commit-plan.json` replay payload; `scripts/Invoke-CommitPlan.ps1`
   executes that verified payload. Tier 2 writes `.slim/commit-plan.json`
   only through `scripts/Build-CommitPlan.ps1` as part of an explicitly
   requested verified replay.

Rename is one indivisible unit (source + destination).

## Routing

| Step | Owner |
|------|-------|
| Enumerate, secret-scan, and split (Tier 0/1) | Orchestrator via `clion_git_status` + staged diff; inline only |
| Grouping judgment | Orchestrator; `@oracle` opt-in Tier 1 only when ambiguous, off by default Tier 2; grouping only, never review |
| Messages and presentation | Orchestrator per the canonical convention |
| Batch discovery (Tier 2) | Orchestrator via `scripts/Get-CommitPlanBatches.ps1`; orchestrator merges inline if too fragmented; never `@explorer` for grouping |
| Execution after `yes` | Agent via `git commit` one-by-one per commit-path semantics |
| Verified replay (explicit request only) | Agent via `scripts/Build-CommitPlan.ps1` then `scripts/Invoke-CommitPlan.ps1`; never silently |
