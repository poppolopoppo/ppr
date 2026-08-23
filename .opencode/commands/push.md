---
description: Run the git-push-planner skill — pre-flight review of unpushed commits, squash plan, commit-message validation, and pre-push checklist.
---

Launch the `git-push-planner` skill against the current branch's unpushed commits.

{user's optional scope: specific commits, branches, or focus areas follow the command}

The skill handles its own subagent routing (git state via CLion MCP / direct
`bash`, secret-pattern scan via background `@explorer`, squash-pattern
detection via background `@oracle`, commit-message format validation inline).
Do not reimplement the workflow inline — load the skill and follow its
Steps 1–4.

## What this does

Load the `git-push-planner` skill. Follow its Steps 1–5.

## Orchestration notes

- The skill produces an agent-executable squash/rebase plan in
  `.slim/push-plan.json` but **never** runs `git push`, `git rebase`,
  `git reset`, `git cherry-pick`, or `git commit --amend`. An executor
  agent (or the user) replays the squash plan via `git reset --soft` +
  cherry-pick/re-commit workflow; the user retains the final
  `git push --force-with-lease` call as the irreversible trust boundary.
- The executor MUST stash dirty working-tree state before `git reset --soft`
  if the working tree is dirty — failing to do so loses the user's
  uncommitted changes.
- For interactive history browsing before deciding to squash, load the
  `git-log-fast-navigation` skill (`flog` alias). For full-project compile
  verification after the squash plan is finalized, load the `validation`
  skill.

## Sample usage

```
/push                                            # review all unpushed commits
/push origin/main                                # review commits ahead of origin/main
/push focus on the recent CMakeLists.txt commits # steer the squash analysis
```
