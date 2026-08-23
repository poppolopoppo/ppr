---
name: git-commit-planner
description: >
  Analyzes local git modifications and proposes an ordered, atomic commit plan
  with conventional subject lines and descriptive bodies. Use this skill whenever
  the user wants to commit, stage, structure, or review their pending changes —
  even if they only say "help me commit this", "how should I split my changes",
  "write commit messages", or "what should I commit first". Especially valuable
  for C++ projects where changes often span headers, modules, tests, and build
  files that must be committed in dependency order.
---

## Contract

Commit planning analysis. The orchestrator drives it; git status/diff/log
retrieval is delegated to `@explorer` and grouping/dependency-ordering
judgment to `@oracle`. It **never** edits the git index, runs `git add`,
runs `git add -p`, or runs `git commit`; its output is a proposed ordered
commit plan with subject lines, bodies, and a machine-readable staging
artifact (`.slim/commit-plan.json`) that an executor agent can replay.
The user retains the final `git commit` invocation as the irreversible
trust boundary.

# Git Commit Planner

Inspect the working tree, group related changes into atomic commits, and output
an ordered plan where each commit entry has a conventional subject and an
explanatory body.

---

## Step 1 — Gather the diff

Run all three commands; each surfaces different information:

```bash
git status --short          # full file list with staging state
git diff HEAD               # every unstaged+staged change unified
git diff --cached           # staged-only (relevant if user already staged some)
git log --oneline -8        # recent history for tone/scope reference
```

If the repo root is not the working directory, prepend `git -C <path>`.

---

## Step 2 — Analyse and group

Read the **full diff** (hunk by hunk), not just the file list. A single file can
contain multiple unrelated changes — treat each hunk as an independent unit.
Group hunks across files by **what changes together for the same reason**.
Good partitions for C++ projects:

| Heuristic | Commit boundary |
|---|---|
| New/renamed module interface (`*.cppm`, `*.ixx`) | Separate from its implementation |
| Public API change (header, export declaration) | Own commit; note ABI impact |
| Small implementation change in one area | Can bundle related TUs in one commit |
| Large multi-file change across a subsystem | Split into one commit per logical unit |
| Bug fix with a matching test | Fix + test together |
| Refactor with no behaviour change | Isolated commit |
| **Adding a source file to CMakeLists.txt** | Go with its feature — same rule as module exports |
| Infrastructure build change (flag, toolchain, dependency) | Isolated commit after the code it enables |
| Dependency bump (`CPM.cmake`, `vcpkg.json`) | Own commit |
| Tests for a feature | Commit with the feature, never separate |
| Module export lines (`export import :partition;`) | Go with their feature |
| Test registration changes (test module `export namespace`) | Go with their feature |
| Formatting / `clang-format` sweep | Always last, own commit |

**Dependency ordering rule**: if commit B requires a declaration introduced in
commit A, A must appear first. Apply this rule at **hunk granularity** — if a
file has hunks for two independent features, check whether one hunk's symbols
are used by the other before ordering them.

- Modules with zero dependencies go first (macros, platform HAL)
- Then subsystems in dependency order (e.g., memory poison before
  allocators, allocators before data structures that use them)
- UnitTest framework goes right after assert/exception handling
  (other tests depend on it)
- Tests go alongside their feature, never in a test-only commit
- **CMakeLists.txt source entries go with their feature commit** —
  do not batch them in a later build commit
- Infrastructure build/config (flags, dependencies, toolchain) goes last

### Consistent granularity

Every commit should represent exactly **one logical change**. If a subsystem
touches multiple independent concerns (e.g., adding a poisoning utility, an
allocator, and an arena), split into three separate commits — one per concern.
The same applies **within a single file** — unrelated hunks belong in different
commits even if they modify the same file.

A typical commit is **50–300 lines** across 1–5 files. If a commit would
exceed ~400 lines, consider whether it can be split further. This keeps the
history reviewable and makes `git bisect` more effective.

---

## Step 3 — Write each commit entry

### Subject line (≤ 72 characters)

Format: `<component>: <imperative-mood summary>`

**Component** — the file name, module partition, class, or service being changed.
Use the exact name as it appears in the codebase: `Macros.h`, `HAL`,
`StableVector`, `cmake`, `Logger`. Keep it Title Case for files/classes,
lowercase for directories.

**Summary** — imperative mood ("add", "fix", "remove", "drop", "rename"),
no capital first letter, no trailing period.

Examples:
```
cmake: enable LTO and set C++23 standard flag
HAL: add platform thread naming and heap query
UnitTest: add simple filtering and child-run mode
Containers: remove Recycler and fix RingBuffer wrap
```

**No type/scope prefixes** (`feat:`, `refactor:`, `fix:`, `chore:`, etc.) —
the component name is the only prefix.

### Body (wrap at 72 characters, blank line after subject)

Keep the body to **1–3 sentences**. Answer only what is essential:

1. **Why** was this change needed? (one sentence)
2. **What** is the key change? (one sentence)

Skip boilerplate (ABI notes, C++ standard versions) unless the change is
breaking or uses a new compiler feature. Example:

```
UnitTest: add simple filtering and child-run mode

Context stores fail callback, optional filter, and child-run flag.
Tests can use expect_crash/fork. Non-zero exit on failure.
```

Footers (BREAKING CHANGE, Fixes #issue, Co-authored-by) are still accepted
when applicable.

```
Fixes #<issue>
BREAKING CHANGE: <short description>
Co-authored-by: Name <email>
```

---

## Step 4 — Output format

Present the plan as a numbered list. Each entry:

```
## Commit N — <component>: <subject>

**Files**: <comma-separated list of files in this commit>

**Message**:
```
<component>: <subject>

<body (1–3 sentences, hard-wrapped at 72 chars)>

<footers if any>
```

**Notes**: <optional: ordering rationale, ABI warning, review suggestion>
```

After all commits, add a short **Summary** section:
- Total commit count
- Any files left unaddressed (with a reason)
- Any ordering constraints the user must respect
- Any breaking changes flagged

---

## Step 5 — Emit machine-readable plan artifact

Write `.slim/commit-plan.json` alongside the markdown plan. The artifact
is the authoritative source for staging execution; the markdown is the
human-readable view.

### JSON schema

```jsonc
{
  "schema_version": 1,
  "plan_id": "<git rev-parse --short HEAD>-<timestamp-iso8601>",
  "generated_at": "<ISO 8601 timestamp>",
  "pre_staging_snapshot": "<git status --porcelain output at plan time>",
  "pre_staging_hash": "<sha256 of pre_staging_snapshot for drift detection>",
  "commits": [
    {
      "index": 1,
      "component": "Memory",
      "subject": "add ScopedArena watermark tracking",
      "body": "ScopedArena now records...\n\nOne sentence why, one sentence what.",
      "files": [
        "lib/engine/core/Core.Memory.cppm",
        "lib/engine/core/Core.Memory.cpp",
        "lib/engine/tests/core/Test.Memory.cppm"
      ],
      "hunk_refs": [
        {
          "file": "lib/engine/core/Core.Memory.cppm",
          "patch": "@@ -42,6 +42,8 @@\n export import :arena;\n+export import :scoped_arena;\n import std;\n",
          "cumulative": false
        },
        {
          "file": "lib/engine/core/Core.Memory.cpp",
          "patch": "@@ -120,0 +121,15 @@\n+// ScopedArena implementation...\n",
          "cumulative": false
        }
      ],
      "staging_instructions": {
        "mode": "patch-apply",
        "apply_command": "git apply --cached --unidiff-zero <<PATCH\n<concatenated patches from hunk_refs>\nPATCH",
        "fallback_note": "If patch-apply fails, use the save-edit-restore workflow (see Constraints §git-add-p-headless)"
      },
      "message": "<component>: <subject>\n\n<body>\n\n<footers if any>",
      "notes": "Optional ordering rationale or ABI warning"
    }
  ],
  "summary": {
    "total_commits": 3,
    "unaddressed_files": [],
    "ordering_constraints": ["Commit 2 depends on export from Commit 1"],
    "breaking_changes": false
  }
}
```

### Field semantics

- **`plan_id`**: Unique identifier for invalidation. Re-plan if working tree
  drifts from `pre_staging_snapshot`.
- **`pre_staging_snapshot`**: Raw `git status --porcelain` at plan time. The
  executor must compare this to a fresh `git status --porcelain` before
  staging; mismatch means the plan is stale and must be regenerated.
- **`pre_staging_hash`**: SHA-256 of `pre_staging_snapshot` for cheap drift
  detection without re-reading the full snapshot text.
- **`hunk_refs[].patch`**: Unified diff text for this hunk, suitable for
  `git apply --cached --unidiff-zero`. Captured from `git diff HEAD -- <file>`
  at plan time. When a hunk is split from a multi-hunk file, this contains
  only the relevant portion.
- **`hunk_refs[].cumulative`**: `true` when this hunk requires all preceding
  hunks in the same file to already be staged (the cumulative-state rule).
  For most commits this is `false`; it becomes relevant when the executor
  stages commit N after commit N-1 has already been committed.
- **`staging_instructions.mode`**: One of:
  - `"patch-apply"` — preferred: patches are applied via `git apply --cached`.
  - `"manual-edit"` — fallback: the file must be manually edited per the
    save-edit-restore workflow. Used when patch context is too fragile
    (e.g., whitespace-only files, binary-adjacent changes).
- **`staging_instructions.apply_command`**: Ready-to-run shell command for
  the `"patch-apply"` mode. For `"manual-edit"`, this is `null` and the
  executor reads the `fallback_note` instead.
- **`staging_instructions.fallback_note`**: Human-readable instructions for
  when the primary mode fails or is unavailable.

### Staging execution protocol

The executor replays commits in order:

1. Validate `pre_staging_hash` matches current `git status --porcelain`.
   If mismatch, abort and request re-plan.
2. For commit N: reset the index to HEAD (`git reset`), then apply the
   cumulative patches for commits 1..N (all `hunk_refs` from earlier
   commits that touch the same files, plus commit N's own hunks).
   This satisfies the cumulative-state rule: each staged snapshot is
   the superset of all prior feature hunks in shared files.
3. Verify `git diff --cached --stat` matches the commit's `files` list.
4. Present the commit message to the user for `git commit`.
5. After user commits, proceed to commit N+1 (which will stage
   1..N+1 cumulatively — but since 1..N are already committed, only
   N+1's new hunks are unstaged, so the effective staging is correct).

### Artifact lifecycle

- Written on every `/commit` invocation.
- Invalidated when working tree changes (detected via `pre_staging_hash`
  mismatch).
- Not committed to git (`.slim/` is gitignored).
- Overwritten on next `/commit` — no accumulation.

---

## Constraints and edge cases

- **Do not invent** functionality not present in the diff. Every claim in a
  commit message must be traceable to actual lines changed.
- **Analyse at hunk granularity**: scan every hunk in `git diff HEAD` and tag
  each hunk with the concern it belongs to. If two hunks in the same file
  belong to different concerns, they must be split into separate commits.
  Only after this per-hunk labelling can you determine the true commit
  boundaries and dependency order.
- If a single file contains **unrelated changes** (e.g., a bug fix and a
  refactor in the same `.cpp`), note that the user should consider `git add -p`
  to split it, and draft both commit messages anyway. In the commit plan, list
  only the relevant hunks' lines for each commit rather than the whole file.
- **C++20 module projects** commonly have shared files that accumulate changes
  for multiple features: `Core.cppm` (module exports), `CMakeLists.txt` (source
  registration), `Core.Tests.cppm` (test tree registration), `main.cpp` (test
  runner). These files MUST be split with `git add -p` so each hunk goes with
  its feature commit. Call this out explicitly in the plan.
- If `git add -p` is needed but the shell doesn't support interactive mode,
  use the `staging_instructions` in the JSON artifact. Two modes are available:
  **`patch-apply`** (preferred): hunks are encoded as unified diff patches
  in `hunk_refs[].patch`; apply via `git apply --cached --unidiff-zero`.
  This is deterministic, doesn't modify the working tree, and supports
  cumulative staging (apply patches from commits 1..N for commit N).
  **`manual-edit`** (fallback): used when patch context is too fragile.
  Follow the save-edit-restore workflow:
  1. Save a backup of the file
  2. `git checkout -- <file>` to reset to HEAD
  3. Manually edit only the hunk(s) needed for the current commit
     (the `hunk_refs[].patch` text shows exactly which lines to add/remove)
  4. Stage and commit
  5. Restore the backup for the next commit
  6. **Important**: each commit's shared file snapshot must contain the
     **cumulative** changes from ALL features committed so far, not just
     the current feature. For example, if commit 1 adds `:event` to
     `Core.cppm` and commit 2 adds `:timer`, then commit 2's `Core.cppm`
     must have BOTH `:event` and `:timer` exports. The JSON artifact's
     `cumulative` flag and staging protocol handle this automatically.
- For **C++ module partitions** (`:partition` syntax), changes to the primary
  interface unit and its partitions often need to be committed together to keep
  the BMI consistent; call this out explicitly.
- If the diff is **very large** (> ~600 changed lines), summarise the grouping
  strategy first and ask the user to confirm before writing all bodies in full.
- If `git status` shows **merge conflicts** or a detached HEAD, report it and
  stop — do not propose commits until the tree is clean.

---

## Contract (hard rule)

This skill does **NOT** perform code review. Code is presumed already-reviewed
by the time commits are planned. Validation is limited to:
- working-tree / merge-state gates
- hunk-level grouping + dependency ordering
- commit-message format (subject + body shape only)

If the user asks for code review, route to `code-reviewer` instead.

## Tools: prefer CLion MCP for git state queries

CLion MCP (`clion_git_status`, `clion_get_repositories`) is index-backed and
runs in the orchestrator's main lane — no subagent round-trip, no separate
model context, no per-call cost beyond a single C++ tool invocation. Prefer
it over spawning `@explorer` for all read-only git state queries
(file enumeration, branch state, dirty tree flags, changed-file lists).
Fall back to direct `bash` (`git rev-parse`, `git log --oneline`,
`git diff --stat`) for full commit content. Fall back to `@explorer` only
when the work is genuinely `rg`/regex over a multi-megabyte stream.

## Subagent routing

| Step | Delegate to | Why |
|------|-------------|-----|
| Repo + changed-file enumeration | orchestrator (in-context, CLion MCP) | `clion_get_repositories`, `clion_git_status` — index-backed, no subagent cost |
| Full `git diff HEAD` content retrieval | orchestrator (in-context, direct `bash`) | Diff content is small enough (C++ projects, 50-400 lines/feature) to read inline |
| Conflict / detached-HEAD gate | orchestrator (in-context, direct `bash`) | `git status --porcelain --branch` blocks push of broken state |
| Grouping + dependency ordering judgment | `@oracle` (background) | Commit-boundary judgment |
| Emit plan | orchestrator | Aggregation + Job Board reconciliation |
| Emit JSON artifact | orchestrator | Writes `.slim/commit-plan.json` with hunk patches, staging instructions, and pre-staging snapshot |

## OMO feature wiring

- **Per-agent `skills`/`mcps` allow-lists** — no subagent skill grants are
  needed. `@oracle` keeps `skills: []`. CLion MCP is called from the main
  lane, not by a subagent.
- **Background orchestration** — launch `@oracle` (grouping) as a single
  background task. CLion MCP + direct `bash` happen inline in the orchestrator
  because they are fast (~1-5s per call) and not context-heavy. Reconcile on
  the Background Job Board. Only split `@oracle` across subsystem zones if
  the diff exceeds ~400 lines across 3+ subsystems (one background oracle
  per zone; reconcile dependency order in main lane).
- **Session reuse** — the orchestrator's recent CLion MCP calls are kept in
  the active turn's context. On a "tweak and re-plan" follow-up, re-run
  `clion_git_status` to detect new/removed/changed files before deciding
  whether a full plan re-issuance is needed. The `.slim/commit-plan.json`
  `pre_staging_hash` provides a machine-readable invalidation check: if the
  hash of a fresh `git status --porcelain` differs from the artifact's
  `pre_staging_hash`, the plan is stale and must be regenerated.
- **`orchestratorPrompt` routing** — trigger on "plan my commits", "how
  should I split my changes", "write commit messages", "review my
  uncommitted changes", "help me commit", "atomic commit plan". Do NOT
  trigger on standalone "commit", "stage", or "save" without context
  about changes/messages.
