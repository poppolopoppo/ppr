---
description: Plan verification evidence for a non-trivial change through the global verification-planning skill.
---

Load the user-global `verification-planning` skill for the requested change:

{user's required scope: feature, bug fix, refactor, or behavior change}

This command depends on that global skill. If it is unavailable, stop and state
that the verification plan cannot be produced until the dependency is installed
or made available; do not recreate its workflow inline.

When available, let the skill own its research, feasibility, and evidence-plan
workflow. It plans verification only: it does not implement, build, or gather
runtime evidence itself. Use it proportionately; small mechanical changes may
use ordinary project checks instead.
