---
name: git-push-planner
description: >
  Pre-flight review skill that inspects local unpushed commits, identifies
  safe history-cleanup opportunities, validates commit messages, and produces
  a user-invoked pre-push plan. Use when the user is preparing to push or
  wants to review, clean up, or validate unpushed history.
---

# Git Push Planner

Run this skill before sending local commits to a remote. It works only on
already-committed history; `git-commit-planner` separately plans commits from
uncommitted changes.

For interactive history browsing, load `git-log-fast-navigation`. For full
project verification after history is finalized, load `validation`.

## Contract

This is a pre-flight history-analysis skill, not a code-review or commit-plan
replay workflow. It never runs `git rebase`, `git reset`, `git commit`, or
`git push`. The user alone invokes any history rewrite and the final push.

The current push plan is written to `.slim/push-plan.json` only when a history
change is proposed. It records the reviewed unpushed history as its
`pre_execution_snapshot`; if fresh history differs, the current plan is stale
and must be regenerated. The artifact describes push-time actions only. It does
not contain, invoke, or replay `.slim/commit-plan.json` payloads.

Validation is limited to:

- branch, upstream, conflict, and working-tree gates;
- history cleanup and ordering judgment;
- commit-message conformance;
- secret-pattern scanning of unpushed diffs; and
- a user-facing pre-push checklist.

Route requests for code review to `code-reviewer` instead.

## Canonical commit-message convention

Validate subjects, bodies, and trailers against the single canonical source:
[`../git-commit-planner/references/commit-message-convention.md`](../git-commit-planner/references/commit-message-convention.md).
Do not duplicate or reinterpret that convention here.

## Routing

| Step | Owner | Responsibility |
|------|-------|----------------|
| Repository and unpushed-history enumeration | Orchestrator | CLion Git state plus Git history commands |
| Secret-pattern scan | `@explorer` background task | Scan unpushed diffs only |
| Cleanup and dependency-order judgment | `@oracle` background task | Decide whether commits remain independent, squash, drop, or reword |
| Message validation and current-plan presentation | Orchestrator | Apply the canonical convention and emit the plan |

## 1. Establish the push range and gates

Determine the range of local commits not present on the tracked upstream. Use
`@{u}..HEAD` when an upstream exists; otherwise compare against the selected
base branch and identify that this is a first push.

```powershell
git status --short --branch
git log --oneline --decorate --graph @{u}..HEAD
git log --stat --oneline @{u}..HEAD
git push --dry-run
```

Stop and report when there are merge conflicts, detached HEAD, no applicable
commits, or the branch is behind its upstream. A dirty tree does not prevent Git
from pushing existing commits, but it blocks a proposed history rewrite: the
user must commit or stash it before invoking any rebase command.

Capture the exact unpushed history in `pre_execution_snapshot` before proposing
changes. A new `/push` invocation always re-enumerates the range; do not reuse
a commit planner's scope or snapshot.

## 2. Judge cleanup opportunities

Read the full diff of each commit in the push range and preserve independently
buildable, reviewable commits whenever possible.

| Pattern | Proposed action |
|---------|-----------------|
| Follow-up `fix build`, `typo`, `wip`, or debug commit | Squash into the commit it corrects |
| Feature followed by its required source/CMake/test registration | Squash when they form one atomic feature |
| Add, then repeated fixes to the same concern | Squash into the original addition |
| A commit and its complete revert inside the range | Drop both when no later commit depends on them |
| Independent dependency bump or broad formatting sweep | Keep separate |
| Independent concerns in the same subsystem | Keep separate |

When reordering is proposed, foundations must precede consumers. Never propose
rewriting commits that are already public/shared without warning about
coordination and a possible force-with-lease push.

## 3. Validate surviving messages and secrets

Validate only commits that survive the proposed cleanup against the canonical
commit-message convention. Check subject length and shape, body separation and
line wrapping, trailing whitespace, and required trailers. Do not apply
Conventional Commit rules.

Scan the full unpushed diff for likely credentials:

```powershell
git log -p @{u}..HEAD | rg -i '(api[_-]?key|secret|password|token|credential|bearer|private[_-]?key)' -C 1
```

If a result is an actual credential rather than code or documentation about a
credential, stop: the user must remove and rotate it before pushing.

## 4. Present the user-invoked push plan

Present a concise plan containing:

1. the push range and `pre_execution_snapshot` freshness condition;
2. ordered `pick`, `squash`, `drop`, and `reword` actions, with replacement
   full messages where needed;
3. the user command for an interactive rebase when the plan changes history;
4. a statement that no rewrite is needed when every action is `pick`; and
5. the pre-push checklist below.

For a rewrite, provide the base commit immediately before the push range and
the user-invoked command:

```powershell
git rebase -i <base-hash>
```

Do not describe `git reset --soft`, patch application, temporary indexes, or
any commit-plan replay method as a push-plan action.

## 5. Emit or refresh the current push plan

When the plan changes history, atomically replace `.slim/push-plan.json`; it is
the current, user-invoked push plan and is not committed. Include the plan ID,
the planned base, `pre_execution_snapshot`, the ordered action list, proposed
messages, and checklist. The snapshot is a freshness guard for the reviewed
push range, not a replay payload or rollback instruction.

Before the user invokes a proposed rebase, they must compare a fresh unpushed
history enumeration with `pre_execution_snapshot`. If it differs, discard the
current plan and run `/push` again. If no history change is needed, report that
result without writing a push-plan artifact.

## Pre-push checklist

| # | Check | User command or confirmation | Critical? |
|---|-------|------------------------------|-----------|
| 1 | Tree is clean before a planned rewrite | `git status --short` | Yes for rewrites |
| 2 | Branch is current with upstream | Fetch, then confirm the intended push range | Yes |
| 3 | Current plan snapshot still matches | Re-run the range enumeration | Yes for rewrites |
| 4 | Surviving messages follow the canonical convention | Step 3 review | Yes |
| 5 | No credentials appear in the push range | Step 3 scan | Yes |
| 6 | Relevant build and tests pass | Run the applicable validation commands | Yes |
| 7 | Dry-run succeeds | `git push --dry-run` | Yes |
| 8 | Branch protections and merge strategy are understood | Confirm repository requirements | Conditional |

Adapt validation commands to the affected platform and targets. Use the
`validation` skill when a full project validation pass is required.

## Edge cases

- For a first push with no upstream, identify the intended remote and branch;
  the user may invoke `git push --set-upstream origin <branch>` after checks.
- If the local branch is behind, the user must integrate remote changes before
  a new push plan is made.
- If the range is large, summarize the proposed cleanup before detailing
  message rewrites.
- For fork workflows, identify that the push target is the fork and that a PR
  is required to contribute upstream.
