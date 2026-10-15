---
name: implement-team
description: Implement an accepted plan using a coordinated team with Sol implementation, Luna exploration and validation, and independent Astra review. Use when the user requests this delegated implementation workflow after planning.
---

# Implement with a team

Continue the accepted plan from the current chat. If the plan is referenced elsewhere, read it before delegating. If no accepted plan is available, resolve that missing input before implementation. Follow explicit user overrides and the project's instructions and specialist workflows.

## Responsibilities

The current main agent remains the coordinator and planning owner, using its current model. It owns scope, design decisions, work sequencing, and the final acceptance decision.

| Responsibility | Model | Reasoning effort |
| --- | --- | --- |
| Implementation and integration | `gpt-6.1-sol` | `high` |
| Repository exploration, test execution, formatting, and requested profiling | `gpt-6-luna` | `medium` |
| Independent correctness review | `gpt-6-astra` | `high` |

Use subagents within the current chat. Set model and effort through the delegation tool's actual parameters, not merely in the task prompt. When overrides require fresh context, provide the accepted plan excerpt and relevant project instructions explicitly. If a required model or delegation capability is unavailable, explain the limitation and obtain agreement before substituting the workflow.

## Implementation and handoffs

- Give implementation the accepted plan, bounded edit scope, facilities to reuse, required behavior, and relevant error and lifetime constraints. The implementer owns routine implementation details and integration, and must consult the coordinator before deviating from the plan.
- A proposed architectural substitution or unexpected feasibility blocker returns to the coordinator with evidence before affected production edits continue. The coordinator resolves decisions within the existing authorization and asks the user only when a necessary decision falls outside it.
- Keep one implementation owner per overlapping edit scope. Coordinate formatting after an edit handoff so it does not race with implementation.
- Reuse agent contexts. Hand off coherent phases and concrete blockers rather than routing every routine step through the coordinator.

## Validation and review

- Keep builds serialized under one execution owner. Follow the project's test and profiling exclusions; avoid competing workloads when collecting timings.
- Before expensive runs, establish the applicable commands, targets, filters, repetition count, runtime environment, and timeout strategy. Escalate unresolved infrastructure problems to the coordinator with the command, error, and attempted remedies.
- Reuse passed checks until changes invalidate them. Follow the accepted scope for profiling and benchmarking. Performance comparisons require compatible workloads, configurations, and known binary/source provenance; qualify weaker evidence.
- Use one persistent independent reviewer for coherent completed behavior and targeted follow-up fixes. Reviewers must not edit files or run formatting, builds, or tests. The implementation owner applies fixes; the execution owner runs checks. Stable source review may overlap validation.
- The coordinator reconciles review findings and execution evidence before reporting completion, identifying any failed, blocked, or unrun acceptance checks.
