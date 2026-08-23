---
description: Run the verification-planning skill — build an evidence path for a non-trivial change before implementing it.
---

Launch the `verification-planning` skill for the proposed change.

{user's required scope: the feature, bug fix, refactor, or behavior change to plan verification for}

The skill handles its own subagent routing (research into unfamiliar
dependencies via `@librarian`, codebase invariant/inputs scan via
`@explorer`, feasibility + cost review via `@oracle`). Do not reimplement
the workflow inline — load the skill and follow its Steps 1–6.

## What this does

1. Frame the claim — state the behavior that needs to become true, the
   conditions that could make a confident conclusion wrong, and the
   important failure modes.
2. Design the evidence path — derive possible paths from the system's
   controllable inputs, observable effects, state transitions, invariants,
   boundaries, and ability to repeat or reverse a scenario.
3. Create a verification affordance when needed — the smallest capability
   that makes the relevant state controllable, observable, repeatable, and
   diagnosable for an agent.
4. Research when the path is unknown — ask `@librarian` for focused research
   on unfamiliar dependencies, frameworks, or rapidly changing capabilities.
5. Make the path runnable — prepare only the support needed to follow the
   evidence path reliably.
6. Close the evidence path — after implementation, follow the planned path
   and interpret the resulting evidence against the original claim.

## Orchestration notes

- The orchestrator drives the skill; research and feasibility review run in
  background subagents. The skill **never** builds or edits to gather
  evidence itself — it plans the path, then later work follows it.
- Use this skill proportionately. Small mechanical changes can follow
  ordinary project checks directly. For larger multi-phase work, let this
  skill establish the evidence path that later work follows.
- Persist the evidence plan in `.slim/verification-plan.json` across
  commits/PRs so the plan survives session boundaries.

## Sample usage

```
/plan add a new Slab allocator with poison-on-free
/plan refactor RawChannel to use a lock-free MPSC ring
/plan port the Windows HAL timer to Linux
```
