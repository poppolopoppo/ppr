---
name: git-push-planner
description: >
  Pre-flight review skill that runs before pushing local unpushed commits.
  Inspects the commits about to be sent to the remote, identifies redundant or
  similar commits that should be squashed, validates each commit message against
  PPR and GitHub conventions, and produces a pre-push validation checklist.
  Use this skill whenever the user is about to push, says "let me push",
  "should I push", "review my commits before pushing", "clean up my history
  before push", or any variant of preparing to send local commits upstream.
triggers: push, push commits, send commits, pre-push, before push, clean history, squash commits
---

# Git Push Planner

Run this skill whenever local commits are about to be sent to a remote repository.
It inspects the unpushed commits, proposes a squash plan to clean up redundant or
duplicate changes, validates commit message quality, and produces a pre-push
validation checklist grounded in GitHub best practices.

This skill is the **push-time counterpart** to `git-commit-planner` (which plans
how to *create* commits from uncommitted changes). This skill works on
**already-committed** history.

For interactive history browsing, load the `git-log-fast-navigation` skill —
its `lg` and `flog` aliases make scanning unpushed history faster than
`git log`. For full-project compile verification across all build
configurations, load the `validation` skill after the push plan is finalized.

---

## Step 1 — Identify the commits about to be pushed

Determine which commits exist locally but not yet on the remote counterpart.
Run all three commands; each surfaces different information:

```bash
# The unpushed commits (replace <branch> with actual branch name)
git log --oneline --decorate --graph --cherry-pick origin/main
# or, if on a feature branch tracking a remote:
git log --oneline --decorate --graph @{u}..HEAD
# or, to compare against the default branch:
git log --oneline --decorate --graph origin/main..HEAD
```

```bash
# Confirm what `git push` would do (dry run — safe, no network write)
git push --dry-run
# If the branch has no upstream yet:
git push --dry-run --set-upstream origin <branch>
```

```bash
# Stat summary of each unpushed commit (file counts, line deltas)
git log --stat --oneline origin/main..HEAD
```

If `git status` shows a dirty working tree (uncommitted changes) or staged-but-
not-committed changes, warn the user: those will **not** be pushed. They must
either commit or stash them before pushing.

If `git log` shows the local branch is **behind** the remote (unpulled commits
exist upstream), advise the user to pull first (preferably with `--rebase`)
before proceeding. Do not propose a push plan while behind.

---

## Step 2 — Analyze commit history for squashing opportunities

Read the **full diff** of each unpushed commit. Group commits by concern and
identify candidates for consolidation.

### Squash triggers

| Pattern | Action |
|---|---|
| "fix typo" / "fix build" / "oops" / "wip" / "debug" | Squash into the commit it corrects |
| Multiple commits in the same component that build toward one feature | Squash into a single logical commit |
| "update CMakeLists.txt" as a separate commit from the feature it adds | Squash into the feature commit |
| Commit reverts or undoes a previous commit in the same push range | Delete both (they cancel out) |
| Commit touches only comments/formatting and precedes/follows a logic commit in the same file | Squash into the logic commit |
| "add X" then "fix X" then "fix X again" | Squash all three into "add X" |

### When NOT to squash

| Pattern | Reason |
|---|---|
| Each commit is independently buildable and passes tests | Preserve bisectability |
| Commit introduces a feature; next commit adds a test for that feature | Tests go *with* the feature, not separate |
| Commit is a dependency bump (CPM.cmake, vcpkg.json) | Own commit — isolates version changes |
| Commit is a formatting/clang-format sweep across many files | Isolated commit, never mixed with logic |
| Two commits touch the same subsystem but are logically independent | Keep separate for review granularity |

### Dependency ordering check

Before squashing, verify the dependency ordering rule (same as `git-commit-planner`):
if commit B depends on a declaration introduced in commit A, A must appear before B.
This is rarely violated in already-committed history, but verify it when proposing
reorderings during rebase.

---

## Step 3 — Review each commit message

Validate the commits that will remain **after** the proposed squash plan.
Cross-reference against PPR's own convention (from `git-commit-planner` skill) and
GitHub best practices.

### PPR commit message convention (from git-commit-planner)

**Subject line** (≤ 72 characters):
```
<component>: <imperative-mood summary>
```
- **Component** — exact name as it appears in the codebase: `HAL`, `cmake`, `Core.Memory`,
  `UnitTest`, `Macros.h`. Title Case for files/classes, lowercase for directories.
- **Summary** — imperative mood ("add", "fix", "remove"), no capital first letter, no trailing period.
- **No Conventional Commit type prefixes** (`feat:`, `fix:`, `refactor:`, `chore:`) —
  the component is the only prefix. (PPR deviates from ConCom here.)

**Body** (optional, wrap at 72 chars, blank line after subject):
- 1–3 sentences answering: (1) Why? (2) What?
- Skip boilerplate unless breaking.

### GitHub best-practice validation

For each commit, check these GitHub-recommended rules:

| Rule | Check | Fail signal |
|---|---|---|
| **Imperative mood** | "If applied, this commit will _<subject>_" reads naturally | "Fixed" / "Added" / "Changed" |
| **50-character subject** | Subject is ≤ 72 chars (PPR rule), ideally ≤ 50 for clean `git log --oneline` | Subject over 72 chars |
| **Blank line after subject** | Body is separated by exactly one blank line | No blank line, or multiple blank lines |
| **Body wrap at 72** | Each body line ≤ 72 chars | Lines exceeding 72 chars |
| **No trailing whitespace** | Subject and body have no trailing spaces | `git log --check` would warn |
| **No secret leakage** | Diff does not contain API keys, passwords, tokens | `rg` scan of diff for key patterns |
| **Signoff (if required)** | `Signed-off-by:` trailer present if project uses DCO | Missing signoff when project requires it |

> **Note on Conventional Commits:** GitHub's ecosystem tools (CHANGELOG generation,
> semantically-release, PR auto-labeling) typically expect Conventional Commits
> (`feat:`, `fix:`, etc.). PPR overrides this with its own `<component>:` convention.
> This skill validates against **PPR's convention** as primary. If the project ever
> adopts Conventional Commits, adapt accordingly.

### Secret scanning

Scan all unpushed diffs for common secret patterns:
```bash
# Quick scan for obvious secrets in unpushed commits
git log -p origin/main..HEAD | rg -i '(api[_-]?key|secret|password|token|credential|bearer|private[_-]?key)' -C 1
```
If any hits appear in real credentials (not just variable names or code that
*handles* secrets), flag and abort — do not push.

---

## Step 4 — Produce the cleaned push plan

Output the final plan: the exact `git rebase -i` todo list (or squash plan),
the revised commit messages, and the pre-push validation checklist.

### Output format

#### A. Squash / rebase plan

Present an interactive-rebase todo table. If no squashing is needed, state
"No changes needed — history is clean."

```markdown
## Squash / Rebase Plan

| Step | Command | New Commit Message |
|------|---------|--------------------|
| 1 | `pick <hash> <short-msg>` | *(unchanged)* |
| 2 | `squash <hash> <short-msg>` | *(squashed into #1)* |
| 3 | `pick <hash> <short-msg>` | `<component>: <new subject>` |
```

**Interactive rebase command to run:**
```bash
git rebase -i <base-hash>
```
where `<base-hash>` is the commit just before the first unpushed commit
(i.e., `origin/main`).

> If the interactive shell does not support full-screen editors, provide a
> non-interactive alternative:
> ```bash
> # Non-interactive squash-and-fixup workflow (for CI or headless)
> git reset --soft origin/main
> # (stage selectively if needed)
> git commit -m "<component>: <subject>" -m "<body>"
> ```

#### B. Revised commit messages

For each commit that survives the squash plan, output the full proposed message:
```
<component>: <subject>

<body>

<footers if any>
```

#### C. Pre-push validation checklist

Before running `git push`, the user must confirm these checks pass:

| # | Check | Command | Critical? |
|---|-------|---------|-----------|
| 1 | Working tree is clean | `git status --short` | Yes — dirty tree blocks push of intended changes |
| 2 | Local branch is up-to-date with remote | `git fetch && git log --oneline origin/main..HEAD \| wc -l` matches expectation | Yes — behind means need to pull first |
| 3 | Commit messages pass review | *(from Step 3)* | Yes |
| 4 | No secrets in diff | *(from Step 3 secret scan)* | Yes — abort if any found |
| 5 | Build succeeds | `cmake --build --preset msvc-dev --target EngineCore` | Yes |
| 6 | Core tests pass | `cmake --build --preset msvc-dev --target EngineCoreTests && ctest --preset msvc-dev` | Yes |
| 7 | App tests pass (if app code changed) | `cmake --build --preset msvc-dev --target EngineAppTests && ctest --preset msvc-dev` | Conditional |
| 8 | ASAN clean (debug builds) | Run tests with ASAN enabled; watch for heap-use-after-free, leaks | Yes — debug builds have ASAN auto-enabled via PPR_ENABLE_DEVELOPER_MODE |
| 9 | Dry-run push succeeds | `git push --dry-run` | Yes |
| 10 | Branch protection rules satisfied | Verify: required status checks, required reviews, signed commits, linear history (if enforced) | Conditional — check repo settings |
| 11 | Merge strategy confirmed | Squash & merge vs. rebase & merge vs. merge commit — confirm which the repo default is | Conditional |

> Adapt check #5–#8 to the appropriate preset for your platform
> (`msvc-dev`, `clang-cl-dev`, `clang-dev`, `gcc-dev`). See the `build-system`
> skill for preset details. For full-project validation across **all** build
> configs, load the `validation` skill after the squash plan is finalized —
> it compiles every platform-relevant configuration in parallel.

---

## Constraints and edge cases

- **Never push if the working tree is dirty.** The user must commit or stash
  first; uncommitted changes are not transmitted by `git push`.
- **Never rebase shared/public history.** If the commits about to be pushed have
  already been pushed (i.e., the branch is publicly shared), warn that rewriting
  history will require a forced push and coordination with collaborators. Prefer
  adding new fixup commits over rewriting.
- **If the branch is behind remote:** advise `git pull --rebase` first, resolve
  any conflicts, then re-run this skill.
- **If merge conflicts exist in the working tree:** report and stop — do not
  propose a push plan until the tree is clean.
- **If the remote branch does not exist yet** (first push of a new feature branch):
  set upstream tracking with `git push --set-upstream origin <branch>` and verify
  that the branch name follows project conventions.
- **Fork-based workflows:** if the local remote is a fork (not `poppolopoppo/ppr`),
  note that the push target is the fork, and a PR will be needed to upstream.
  Remind the user to push to a uniquely-named branch to avoid clobbering.
- **Large diffs (>600 changed lines):** summarize the squash strategy first and
  ask the user to confirm before proposing detailed commit message rewrites.
- **Conventional Commits vs. PPR convention:** PPR uses `<component>:` prefixes,
  not `feat:`/`fix:`. Do not flag PPR-style subjects as violations of Conventional
  Commits. If the project configuration (e.g., `release-please` or semantic-release)
  requires Conventional Commits, note that PPR has explicitly opted out.
- **Cross-skill workflow:** browse history with `git-log-fast-navigation`'s `flog`
  alias before deciding to squash. Run the `validation` skill for full-project
  compile verification across all configs after rebasing.

## Orchestrator & OMO Integration

**Contract:** Pre-push analysis. The orchestrator drives it; git queries go to `@explorer`, message/secret validation to `@oracle`/`code-reviewer`. It never runs `git push`.

### Subagent routing
| Step | Delegate to | Why |
|------|-------------|-----|
| `git log origin/main..HEAD`, `--dry-run`, stat | `@explorer` | Isolated shell |
| Squash-pattern + message validation | `@oracle` / `code-reviewer` | Convention checks |
| Emit checklist | orchestrator | Aggregation |

### OMO feature wiring
- **Per-agent `skills`/`mcps` allow-lists** — `@explorer` limited to `git log/diff/push --dry-run` + `rg` secret scan.
- **Session reuse** — cache `origin/main..HEAD` output; re-scan only new commits after rebase.
- **`orchestratorPrompt` routing** — trigger on 'push', 'should I push', 'review my commits before pushing'.
