---
description: Run the code-reviewer skill against the current git diff
---

Launch the `code-reviewer` skill against the current git changes.

{user's optional scope: specific files or focus areas follow the command}

The skill owns scope, evidence collection, findings, and its resolution gate.
Do not restate its workflow here.

## Sample usage

```
/review                                          # review all unstaged+staged changes
/review lib/engine/core/Core.Memory.cppm         # review specific files only
/review focus on ASAN poison lifecycle           # steer the review to a concern
```
