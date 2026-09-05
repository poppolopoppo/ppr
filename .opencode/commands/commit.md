---
description: Plan atomic commits, fast-path by default, commit after single yes.
---

Delegate `/commit` to the `git-commit-planner` skill. Fast-path is the
default: inline split, PPR messages, single `yes`, direct `git commit`.
Full verified replay applies only over-cap on explicit request.

Forward any optional text after `/commit` unchanged. The skill owns its
interpretation.

```text
/commit
/commit lib/engine/core/memory
/commit lib/engine/core/Core.Memory.cppm
```
