---
description: Run the code-reviewer skill against the current git diff
---

Launch the `code-reviewer` skill against the current git changes.

{user's optional scope: specific files or focus areas follow the command}

The skill handles its own subagent routing (diff retrieval via `@explorer`,
dimension reviews via background `oracle` subagents, per-finding validation,
resolution gate via `@fixer`/`@oracle`). Do not reimplement the workflow
inline — load the skill and follow its Steps 1–7.

## Sample usage

```
/review                                          # review all unstaged+staged changes
/review lib/engine/core/Core.Memory.cppm         # review specific files only
/review focus on ASAN poison lifecycle           # steer the review to a concern
```
