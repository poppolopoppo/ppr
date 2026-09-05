---
description: Plan atomic commits and execute only after explicit verified plan-ID confirmation.
---

Delegate `/commit` to the `git-commit-planner` skill. Follow that skill's
scope gate, routing, message convention, and confirmation-gated execution
contract; do not add planning or execution rules here.

Forward any optional text after `/commit` unchanged. The skill owns its
interpretation.

```text
/commit
/commit lib/engine/core/memory
/commit lib/engine/core/Core.Memory.cppm
```
