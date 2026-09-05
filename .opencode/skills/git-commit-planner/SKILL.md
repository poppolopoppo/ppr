---
name: git-commit-planner
description: >
  Plans atomic commits, fast-path by default, full verified replay only
  over-cap. Use when the user wants help splitting changes, writing commit
  messages, or preparing commits.
---

# Git Commit Planner

## Contract

Planning is non-mutating. `/commit` has `git diff --cached` semantics:
analyze only staged hunks, including staged hunks of partially staged
files. Inputs are literal file or directory paths, never pathspecs,
globs, or magic. Never push, amend, or force-push in `/commit`; refuse
`--no-verify`, `--amend`, and any force-push flag.

Use the shared [commit-message convention](references/commit-message-convention.md).
PPR convention wins unconditionally over any other style. Every claim
must trace to a changed line.

## Tier 0 — fast path (<=10 paths, single concern)

1. `clion_git_status` to record branch state and staged files.
2. Inline heuristic split by the orchestrator (no `@oracle`).
3. Draft PPR-convention message(s) for the single concern.
4. Present files + full message, exit on zero changes or conflicts.
5. Single `yes` confirmation, then direct `git commit` with an
   explicit literal path list.

No `@oracle`, no `.slim/*.json`, no `scripts/` in this tier.

## Tier 1 — multi-commit (11-15 paths or 2-3 concerns)

Same as Tier 0, plus an in-memory multi-commit plan: ordered list of
paths + messages kept in conversation for push-planner reuse. `@oracle`
is opt-in only when grouping is genuinely ambiguous.

## Tier 2 — over-cap (over 15 paths or 204800 bytes)

1. Run `Get-CommitPlanBatches.ps1`, present at most 8 chunks.
2. User picks one chunk; that chunk drops to Tier 0/1.
3. Verified replay via `Build/Invoke-CommitPlan.ps1` with
   `ConfirmedPlanId` + freshness re-check only when the user
   explicitly asks for it.

A rename is one indivisible unit (source + destination). If one unit
cannot fit the caps, require a narrower selection.

## Routing

| Step | Owner |
|------|-------|
| Enumerate and split (Tier 0/1) | Orchestrator via `clion_git_status` |
| Grouping judgment | Orchestrator; `@oracle` opt-in Tier 1/2 only |
| Messages and presentation | Orchestrator |
| Batch discovery (Tier 2) | Orchestrator, `Get-CommitPlanBatches.ps1` |
| Execution after `yes` | Agent via direct `git commit` |
| Verified replay (explicit request) | Agent via `Build/Invoke-CommitPlan.ps1` |
