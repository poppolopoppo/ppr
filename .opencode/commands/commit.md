---
description: Run the git-commit-planner skill — analyze local git modifications and propose an ordered, atomic commit plan with conventional subject lines and descriptive bodies.
---

Launch the `git-commit-planner` skill against the current working tree.

{user's optional scope: specific files, components, or focus areas follow the command}

The skill handles its own subagent routing (git state via CLion MCP / direct
`bash`, grouping + dependency-ordering judgment via background `@oracle`,
plan emission in the orchestrator). Do not reimplement the workflow inline —
load the skill and follow its Steps 1–4.

## What this does

1. Gather the diff (`git status --short`, `git diff HEAD`, `git diff --cached`,
   `git log --oneline -8`).
2. Analyze at hunk granularity and group related changes into atomic commits
   in dependency order.
3. Write each commit entry with a `<component>: <imperative-mood summary>`
   subject (≤ 72 chars) and a 1–3 sentence body (wrap at 72 chars).
4. Emit a numbered plan with files, messages, and ordering rationale.
5. Write `.slim/commit-plan.json` — a machine-readable artifact containing
   per-hunk patches, staging instructions, and a pre-staging snapshot for
   drift detection.

## Orchestration notes

- The orchestrator plans and delegates; git state queries run inline via CLion
  MCP / `bash`. Grouping judgment runs in a background `@oracle` task.
- The skill produces an **agent-executable staging plan** in
  `.slim/commit-plan.json` but **never** runs `git add`, `git add -p`, or
  `git commit`. An executor agent (or the user) replays staging instructions
  per-commit; the user retains the final `git commit` call as the
  irreversible trust boundary.
- For C++20 module projects, shared files (`Core.cppm`, `CMakeLists.txt`,
  test registration) often accumulate changes for multiple features. The
  JSON artifact encodes per-hunk patches and cumulative staging semantics so
  each hunk goes with its feature commit without manual `git add -p`.

## Sample usage

```
/commit                                          # plan all unstaged+staged changes
/commit lib/engine/core/Core.Memory.cppm         # plan changes in specific files
/commit focus on the new arena allocator         # steer the grouping to a concern
```
