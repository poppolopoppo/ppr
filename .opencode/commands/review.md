---
description: Parallel code review using multiple @code-review subagents with correlated summary
---

Review the current git changes using FOUR (4) @code-review subagents running in parallel.

{user's optional scope: specific files or focus areas follow the command}

## Focus areas

Each subagent receives the SAME diff context but focuses on a different dimension:

| Agent | Focus |
|-------|-------|
| 1 — Security & memory | ASAN violations, use-after-free, buffer overflows, poison lifecycle, `noexcept` safety |
| 2 — Performance & cache | Hot path allocations, false sharing, cache line layout, branch ordering, `[[likely]]` |
| 3 — Correctness & edge | Invariants, preconditions, boundary values, error paths, state mutation, iterator validity |
| 4 — Conventions & style | AGENTS.md compliance, naming, `constexpr`, `noexcept`, module structure, CMake hygiene |

## Output format

Each subagent outputs structured findings with:

```
Severity: ❌ Error | ⚠️ Warning | 💡 Suggestion
Dimension: <one of the above>
File: path:line
Issue: <one sentence>
Rationale: <cite AGENTS.md section or C++ best practice>
```

## Final correlation

After all four subagents complete, read their outputs and produce a single
ranked summary grouped by severity. Flag any finding that appears in
multiple reviews as high confidence.

## Sample usage

```
/review                                          # review all unstaged+staged changes
/review lib/engine/core/Core.Memory.cppm         # review specific files only
/review focus on ASAN poison lifecycle           # steer the review to a concern
```
