---
name: git-commit-planner
description: >
  Plans atomic commits, fast-path by default, full verified replay only
  over-cap. Use when the user wants help splitting changes, writing commit
  messages, or preparing commits.
---

# Git Commit Planner

## Contract

Planning (Tiers 0-2) is non-mutating. An explicit instruction to commit
authorizes execution within the requested scope; otherwise present the plan
and obtain approval once. Execution commits one-by-one in logical order. `/commit` has
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

Analysis budget: `hunk skim` only. Use native `git status --short`,
`git diff --cached --stat`, and a skim of staged hunks sufficient to group concerns.
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

1. Native Git status + staged diff stat + hunk skim; apply hygiene
   gates (exit on zero staged, stop on conflicts).
2. Orchestrator inline secret scan of the staged diff before drafting;
   on an actual credential hit stop and require removal and rotation
   before any draft:
   ```sh
   git diff --cached -p | rg -i '(api[_-]?key|secret|password|token|credential|bearer|private[_-]?key)' -C 1
   ```
3. Orchestrator inline heuristic split (no `@oracle`, no `@explorer`).
4. Draft message per the canonical convention; present files + message.
5. With commit authorization, run `git commit` per commit-path semantics above.

No `@oracle`, no `@explorer`, no `.slim/*.json`, no `scripts/` in this
tier.

## Tier 1 — multi-commit (delta on Tier 0)

Tier 0 procedure, plus an in-memory ordered multi-commit plan (paths +
messages) for push-planner reuse. Triggers on EITHER heuristic
condition: <=10 paths with 2+ concerns, or 11-15 paths with any concern
count. `@oracle` opt-in only when grouping is genuinely ambiguous, for
grouping judgment only. Tier 1 never writes `.slim/commit-plan.json`.

## Tier 2 — over-cap (over 15 paths or 204800 bytes `git diff --cached` size heuristic)

1. Batch discovery: run the bundled [commit_plan.py discover](scripts/commit_plan.py), present at
   most 8 chunks. If too fragmented (>8 chunks, single-file splinters,
   or chunks violating the atomicity heuristic), the orchestrator merges
   small batches into bigger logical commits inline via
   native Git status + staged diff stat + hunk skim. Never delegate
   grouping to `@explorer`. `@oracle` off by default; opt-in only for
   genuinely ambiguous grouping judgment.
2. Order along the PPR layer order (`engine.core` -> `engine.math` /
   `engine.shader` -> `engine.rhi` -> `engine.app` -> `app.game`;
   see `AGENTS.md`). Refactor before feature. Mechanical bulk (renames,
   formatting, codemods, generated) in its own commit. Tests travel with
   the code they validate. Setup is folded into the task that needs it.
   Path-count and byte thresholds above are heuristics, not hard gates.
3. Present the ordered multi-commit sequence; use existing commit authorization
   or obtain approval once, then commit one-by-one in logical order per
   commit-path semantics. Stop on first failure (hook, conflict,
   error): abort the sequence, report remaining uncommitted.
4. Verified replay only via the bundled [commit_plan.py replay](scripts/commit_plan.py) on explicit
   user request, never silently. [commit_plan.py build](scripts/commit_plan.py) builds
   the `.slim/commit-plan.json` replay payload; the bundled replay script
   executes that verified payload. Tier 2 writes `.slim/commit-plan.json`
   only through the bundled builder as part of an explicitly
   requested verified replay.

Rename is one indivisible unit (source + destination).

## Bundled helper paths and scope

Markdown links under `scripts/` and `references/` are relative to this skill's
directory, not the repository root. Run helpers from the repository root so Git
and `.slim/` paths resolve correctly; pass the full repository-relative script path:

```sh
python .opencode/skills/git-commit-planner/scripts/commit_plan.py discover
```

Discovery defaults to staged changes and does not mutate the index. Its JSON
describes path/byte batches, not semantic commit boundaries. `--all-local` also
reads unstaged tracked changes and rejects matching untracked files; do not use
it as a substitute for staging the authorized scope. The helper's per-unit caps
may reject a large indivisible file/rename; ordinary commit grouping can proceed
inline because those caps are artifact limits, not a limit on committing work.

All helper commands use Python 3.10+ and Git on both Windows and Linux;
PowerShell is not required. Here `python` means the Python 3.10+ interpreter
(`python3` on Linux or `py -3` on Windows if needed). Before execution, verify
the helper exists and check the interpreter version. A missing helper is a broken skill
reference: report it, locate the bundled resource, and correct the reference;
do not invent a repository-root path or claim it was deleted without history.
For an explicitly requested replay, inspect the parameters and schema below, build the payload, verify/dry-run it,
and supply the exact verified plan ID for execution. Ordinary commits do not
require replay artifacts.

## Python helper interface

Use `discover --paths path/to/file path/to/directory` for a literal selection;
quote paths containing spaces. Discovery emits up to eight candidates without
changing the index. Rename/copy endpoints stay together. Grouping JSON must list
exact file paths (both endpoints for renames), rather than directory selections:

```json
{
  "commits": [
    {
      "files": ["path/to/file.cpp"],
      "component": "component",
      "subject": "update the behavior",
      "body": "Explain why the change is needed.",
      "content_mode": "staged"
    }
  ]
}
```

`body` is optional; authored paragraph breaks and trailer records are preserved.
`content_mode` defaults to `staged`; `all-local` explicitly captures tracked
working-tree content against HEAD and rejects matching untracked files. Never
switch modes to circumvent staged-only authorization. Optional
`discovery_selection.total_batch_count` carries the selected discovery count.
The schema-v4 plan records HEAD, branch, index fingerprint, ordered messages,
approved/touched paths, exact base64 patches and SHA-256 payload hashes.

For an explicitly requested verified replay, run these commands in order:

```sh
python .opencode/skills/git-commit-planner/scripts/commit_plan.py build --grouping-json .slim/grouping.json
python .opencode/skills/git-commit-planner/scripts/commit_plan.py verify --plan-json .slim/commit-plan.json
python .opencode/skills/git-commit-planner/scripts/commit_plan.py replay --plan-json .slim/commit-plan.json --dry-run
```

Only after replay authorization, execute `replay --plan-json
.slim/commit-plan.json --confirmed-plan-id <verified-sha256>`. The ID hashes the
exact plan bytes. A changed HEAD, branch, index fingerprint or selected content
requires a new plan; verification alone does not establish freshness. Execution
uses an isolated temporary index, retains unrelated staged content and unstaged
hunks, and runs normal Git hooks. It stops at the first failure, reports completed
commit hashes and final status, and retains a failed replay index for diagnosis.
Completed commits are not rolled back. Successful `all-local` steps synchronize
only their selected paths in the original index. Grouping inputs are retained
when building a plan. Discovery/verification never commit.

Run helper regression tests with `python -m unittest discover -s scripts/tests
-p test_commit_plan.py -v`. Tests create disposable repositories; they never
commit or alter the index in the developer checkout. Native Windows execution
needs a Windows host; the executable-hook fixture is POSIX-specific.

## Routing

| Step | Owner |
|------|-------|
| Enumerate, secret-scan, and split (Tier 0/1) | Orchestrator via native Git status + staged diff; inline only |
| Grouping judgment | Orchestrator; `@oracle` opt-in Tier 1 only when ambiguous, off by default Tier 2; grouping only, never review |
| Messages and presentation | Orchestrator per the canonical convention |
| Batch discovery (Tier 2) | Orchestrator via the bundled discovery helper above; merge inline if too fragmented |
| Authorized execution | Agent via `git commit` one-by-one per commit-path semantics |
| Verified replay (explicit request only) | Agent via the bundled builder and replay helpers above; never silently |
