---
description: Multi-model parallel planning with scientific peer review. Spawns three independent planners then cross-reviews and synthesizes results.
---

Plan the following task using the SCIENTIFIC PEER REVIEW method:

**Task:** {user's message after the command name}

---

## Phase 1 — Independent Proposals (parallel)

Spawn THREE (3) `task` subagents (type: `general`) in **parallel**. Each produces a complete,
independent plan. They do NOT communicate with each other.

- A — **Aggressive**: Propose novel, ambitious approaches. Optimize for performance and elegance first.|
- B — **Conservative**: Propose safe, battle-tested approaches. Optimize for reliability and minimal risk.|
- C — **Fresh perspective**: Use a different model or temperature than A and B. Bring an independent viewpoint.|

Each agent writes its proposal to `.opencode/plans/<task-name>-proposal-{a,b,c}.md`
using the standard plan format:

```
## Phase 1 — <title>
### Step 1 — <description>
- Files: <paths>
- Operations: <rename/edit/create>
```

---

## Phase 2 — Cross Review (parallel)

Spawn THREE (3) `task` subagents (type: `general`) in parallel. Each reads ALL three
proposals from Phase 1, then:

1. Identifies strengths and weaknesses of each proposal
2. Flags blind spots or missing considerations
3. Suggests what to adopt from each

Each reviewer writes their analysis to
`.opencode/plans/<task-name>-review-{a,b,c}.md`.

---

## Phase 3 — Synthesis

A final agent reads all 6 files (3 proposals + 3 reviews) and produces:

- `.opencode/plans/<task-name>-synthesis.md` — The final plan combining
  the best elements from all proposals, with:
  - **High confidence** — areas where all reviewers agreed
  - **Needs judgment** — areas where reviewers disagreed, with the
    trade-offs explained

Present the synthesis to the user for approval before switching to Build
mode for execution.

---

## Sample usage

```
/parallel-plan refactor Core.Memory.PagePool to use HugePage allocation
/parallel-plan redesign the event system for lock-free dispatch
/parallel-plan add a chunk-based allocator for the rendering pipeline
```
