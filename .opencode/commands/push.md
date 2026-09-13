---
description: Run git-push-planner to inspect unpushed commits and prepare a safe pre-push plan.
---

Load `git-push-planner` for the current branch's unpushed commits.

{user's optional scope: commits, base branch, or focus area}

Follow the skill's workflow; do not recreate it inline. It inspects history,
checks commit messages and secrets, and writes a proposed plan. It does **not**
run `git push`, `git rebase`, `git reset`, `git cherry-pick`, or
`git commit --amend`, and this command must not promise a reset/replay executor.
The user retains the final push decision and any history mutation boundary.

Use `validation` separately when post-change build/test evidence is needed.
