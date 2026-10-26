---
name: execution-tools
description: Select and verify CLion MCP debugging, official WinDbg MCP, or PIX MCP when investigating native execution, hangs, or CPU/GPU costs in PPR.
---

# Execution Tools

Read [the execution-tools runbook](../../../docs/debugging/execution-tools.md)
before a new native debugging or profiling workflow. It records the verified
capabilities, known failed routes, and the evidence needed to claim success.

- Prefer CLion's native debugger controls for a new source-level live session;
  check the current router schema and executable configuration first.
- Use [windbg-triage](../../../.opencode/skills/windbg-triage/SKILL.md) for the
  official WinDbg session, native crash/dump analysis, and authorized live control.
- Use [pix-profiling](../../../.opencode/skills/pix-profiling/SKILL.md) for CPU
  marker timing or GPU capture. These are different capture and analysis paths.

Keep one execution owner and reuse the task's build/execution authorization.
This skill grants no additional permissions and does not require new demos,
builds, or reproductions when existing evidence answers the question. Never
equate a responsive status tool, a zero launcher exit, or a nonempty capture
file with successful debugging or profiling.
