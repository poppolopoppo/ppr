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

# Git Commit Planner

Inspect the working tree, group related changes into atomic commits, and output
an ordered plan each commit entry has a conventional subject and an explanatory
body.

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
|---|---|---|
| New/renamed module interface (`*.cppm`, `*.ixx`) | Separate from its implementation |
| Public API change (header, export declaration) | Own commit; note ABI impact |
| Small implementation change in one area | Can bundle related TUs in one commit |
| Large multi-file change across a subsystem | Split into one commit per logical unit |
| Bug fix with a matching test | Fix + test together |
| Refactor with no behaviour change | Isolated commit |
| **Adding a source file to CMakeLists.txt** | Go with its feature — same rule as module exports |
| Infrastructure build change (flag, toolchain, dependency) | Isolated commit after the code it enables |
| Dependency bump (`CPM.cmake`, `vcpkg.json`) | Own commit |
| Formatting / clang-format sweep | Always last, never mixed with logic |
| **Tests for a feature** | Commit with the feature, never separate |
| **Module export lines** (`Core.cppm` adds `export import :foo;`) | Go with their feature — use `git add -p` to split |
| **Test registration changes** (`Core.Tests.cppm` adds `_.recurse(foo)`) | Go with their feature — use `git add -p` to split |

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

Footers (BREAKING CHANGE, Fixes #issue) are still accepted when applicable.

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
  recommend this workflow:
  1. Save a backup of the file
  2. `git checkout -- <file>` to reset to HEAD
  3. Manually edit only the hunk(s) needed for the current commit
  4. Stage and commit
  5. Restore the backup for the next commit
  6. **Important**: each commit's shared file snapshot must contain the
     **cumulative** changes from ALL features committed so far, not just
     the current feature. For example, if commit 1 adds `:event` to
     `Core.cppm` and commit 2 adds `:timer`, then commit 2's `Core.cppm`
     must have BOTH `:event` and `:timer` exports.
- For **C++ module partitions** (`:partition` syntax), changes to the primary
  interface unit and its partitions often need to be committed together to keep
  the BMI consistent; call this out explicitly.
- If the diff is **very large** (> ~600 changed lines), summarise the grouping
  strategy first and ask the user to confirm before writing all bodies in full.
- If `git status` shows **merge conflicts** or a detached HEAD, report it and
  stop — do not propose commits until the tree is clean.

## Orchestrator & OMO Integration

**Contract:** Analysis skill. The orchestrator drives it; diff retrieval is delegated to `@explorer` and the grouping/ordering heuristics to `@oracle`. It never edits the index or runs `git commit`.

### Subagent routing
| Step | Delegate to | Why |
|------|-------------|-----|
| `git status`/`diff`/`log` retrieval | `@explorer` | Isolated shell |
| Apply grouping + dependency ordering | `@oracle` | Judgment on commit boundaries |
| Emit plan | orchestrator | Aggregation |

### OMO feature wiring
- **Per-agent `skills`/`mcps` allow-lists** — `@explorer` restricted to `git status/diff/log` (custom agent or allow-list).
- **Background orchestration** — fetch diff in background while orchestrator previews scope.
- **Session reuse** — reuse `@explorer` for incremental diffs after user tweaks.
- **`orchestratorPrompt` routing** — trigger on 'commit', 'stage', 'plan my changes', 'how should I split this'.
