---
name: git-log-fast-navigation
description: >
  Efficiently navigate Git log history using Git log flags, ripgrep (rg), and
  interactive filtering with fzf — with Windows install notes and cross-shell
  .gitconfig aliases. Use when the user wants to scan history, find the commit
  that introduced or removed something, browse recent changes, or explore
  branches interactively.
---

# Git Log Fast Navigation

This skill provides patterns, workflows, and aliases for navigating Git history
using native Git log formatting, **ripgrep (`rg`)**, and **`fzf`**. These tools
make scanning and filtering history faster and more interactive than raw
`git log`.

## Contract

This skill is a reference for choosing and running git/log/fzf pipelines. It
**does not** run shell pipelines directly and **does not** mutate history —
it only reads. The orchestrator decides which pipeline to run, then delegates
execution to `@explorer`; findings are summarized by the orchestrator.

### Subagent routing

| Step | Delegate to | Why |
|------|-------------|-----|
| Run `git log \| fzf`, `rg` searches, branch selectors | `@explorer` | Shell execution isolated to subagent |
| Summarize findings | orchestrator | Planning/aggregation |

## OMO feature wiring

- **Per-agent `skills`/`mcps` allow-lists** — restrict `@explorer` to
  `git`, `rg`, `fzf` via a custom agent or MCP allow-list so no arbitrary
  shell escapes occur.
- **Background orchestration** — kick off the `git log`/`rg` scan in a
  background `@explorer` subagent while the orchestrator previews scope
  (repo size, date range); reconcile on the Background Job Board before
  reporting.
- **Custom agent** — optionally define a `git-nav` custom agent (prompt +
  `orchestratorPrompt`) wrapping these pipelines.
- **Session reuse** — reuse the `@explorer` session to cache the last `fzf`
  selection across repeated queries.
- **`orchestratorPrompt` routing** — trigger on "navigate history", "find
  the commit that…", "show recent changes".

> **Shell note:** The `.gitconfig` aliases in §4 run through Git's bundled
> `sh` and work from PowerShell (pwsh) and CMD on Windows. The inline
> one-liners in §3 use POSIX pipes (`|`); run them from pwsh where `git`
> and `rg` are available directly, and substitute PowerShell pipeline
> equivalents for any POSIX-only utilities (e.g. `xargs`).

---

## 1. Fast Native Log Formatting

Avoid full log dumps by using compact, scan-friendly log formats.

### Compact Graph View

```bash
git log --graph --oneline --decorate -n 30
```

### Formatted Log with Author & Relative Date

```bash
git log --pretty=format:"%C(yellow)%h%Creset %s %C(green)(%cr) %C(bold blue)<%an>%Creset" -n 50
```

---

## 2. Fast Content Search with `ripgrep` (`rg`)

`rg` has a fast regex engine and is handy for filtering piped Git output.
Note: when `rg` reads from a pipe (stdin) it processes a single stream and
runs single-threaded, so it does **not** gain its filesystem multi-threading
advantage here. For pure commit/diff search on large repos, the native
`git log -S<string>` / `git log -G<regex>` / `git log --grep=<term>` are
often faster because they scan Git's object DB server-side without emitting
every patch.

### Search Commit Messages

```bash
git log --oneline | rg -i "search_term"
# faster on large repos (server-side filter):
git log --grep="search_term"
```

### Search Patch / Diff History Across Commits

Filter commit diffs through `rg` with ANSI color preservation. Cap the
history (`-n`) so very large repos don't dump every patch:

```bash
git log -p --color=always -n 200 | rg "search_pattern" -C 3
# native alternative, faster and scales better:
git log -G "search_pattern" -p
```

### Search Files in Current Working Tree

```bash
rg "function_name" --type-add 'code:*.{ts,js,py,cs}' -t code
```

---

## 3. Interactive History Exploration with `fzf`

Piping Git logs into `fzf` creates a terminal-based interactive fuzzy finder
with real-time previews.

> **Quoting:** fzf replaces `{}` / `{1}` tokens with the selected item
> **before** shell parsing, so always wrap them in quotes (`"{}"`, `"{1}"`)
> to avoid word-splitting or injection from filenames/branches containing
> spaces or shell metacharacters. `--ansi` is required for `{1}` field
> extraction so the commit hash isn't polluted by ANSI escapes.

### Interactive Commit Browser with Live Diff Preview

Scroll through commit logs interactively. Selecting a commit and pressing
Enter opens the full diff in `less`.

```bash
git log --oneline --color=always | fzf --ansi --preview 'git show --color=always "{1}"' --bind 'enter:execute(git show "{1}" | less -R)'
```

### Interactive File Diff Finder

Select a file changed in a commit to view its diff:

```bash
git diff-tree --no-commit-id --name-only -r HEAD | fzf --preview 'git diff HEAD -- "{}"'
```

### Interactive Branch Selector

Fuzzy search across local and remote branches sorted by recent commit date.
The selection is fed through `xargs -I{}` with a `--` terminator and a quoted
argument, so branch names containing spaces or starting with `-` are handled
safely:

```bash
git branch -a --format='%(refname:short)' --sort=-committerdate | grep -v '/HEAD$' | fzf --header="Select Branch" | xargs -I{} git checkout -- "{}"
```

---

## 4. Git Configuration Aliases (`.gitconfig`)

Save these aliases directly to your `.gitconfig` (`%USERPROFILE%\.gitconfig`
or `~/.gitconfig`). Git executes shell-snippet aliases (`!`-prefixed) through
its bundled POSIX `sh`, so they are available from Bash, PowerShell, and CMD
alike.

```ini
[alias]
    # Compact visual history
    lg = log --graph --pretty=format:'%C(yellow)%h%Creset -%C(yellow)%d%Creset %s %Cgreen(%cr) %C(bold blue)<%an>%Creset' --abbrev-commit

    # Interactive fzf commit viewer with live diff preview (-n caps history for large repos)
    flog = "!f() { git log --oneline --color=always -n 500 | fzf --ansi --preview 'git show --color=always "{1}"' --bind 'enter:execute(git show "{1}" | less -R)'; f"

    # Fast commit message search using ripgrep (passes $1 as a regex; use -F for literal matches)
    rglog = "!f() { git log --oneline | rg -i "$1"; }; f"

    # Search diff patch content using ripgrep (-n caps history; --color dropped for speed)
    rgdiff = "!f() { git log -p -n 200 | rg -i "$1" -C 3; }; f"
```

---

## 5. Prerequisites & Verification

Ensure `fzf` and `rg` are available in your system `$PATH`:

```bash
rg --version
fzf --version
```

If missing on Windows, install via winget or scoop:

```cmd
winget install BurntSushi.ripgrep.MSVC
winget install junegunn.fzf
```
