---
description: Plan verification evidence for a non-trivial change through the global verification-planning skill.
---

Load the `verification-planning` skill from the current skill catalog for the
requested change:

{user's required scope: feature, bug fix, refactor, or behavior change}

This command requires that skill to be exposed and permitted in this session. If
it is unavailable, report the dependency as blocked; presence in a plugin cache
does not make it callable. Do not recreate its workflow inline.

When available, let the skill own its research, feasibility, and evidence-plan
workflow. It plans verification only: it does not implement, build, or gather
runtime evidence itself. Use it proportionately; small mechanical changes may
use ordinary project checks instead.
