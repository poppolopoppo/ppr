---
description: Analyze local git modifications and propose an ordered, atomic commit plan with conventional subject lines and descriptive bodies.
---

Load the `git-commit-planner` skill and follow its workflow end-to-end: gather the diff (status/diff/log), group related changes into atomic commits, and output an ordered plan where each commit entry has a conventional subject and an explanatory body. Do not stage or commit anything yourself — the output is a proposed plan. $ARGUMENTS