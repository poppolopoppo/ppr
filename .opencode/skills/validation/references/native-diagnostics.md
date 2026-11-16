# Native execution diagnostics

For native debugging/profiling requested during validation, use the
[execution-tools runbook](../../../../docs/debugging/execution-tools.md) before a
new reproduction. Keep status connectivity, target control, useful stack/capture
evidence and application outcome as separate results. Prefer native MCP control
over the known failed RTK/PTY CDB interrupt path. Validate watchdog/cleanup bounds
and preserve failed diagnostic attempts separately from solver/test failures.

## Official WinDbg MCP gate

Applies to WinDbg client wiring, skill/runbook, and crash-preparation changes.
Otherwise record `windbg_gate: not_applicable`.

- Parse changed PowerShell and JSON. Check the `WinDbg` entry invokes the
  supplied official proxy without arguments, grants resolve, and retired daemon
  commands are absent. Check that the executable exists on the Windows host.
- Verify Codex recognizes the new server entry and preserves unrelated settings.
- When a user-owned WinDbg MCP session is available, discover tools, select the
  intended session, and verify `get_target_state`. Configuration or proxy startup
  alone does not prove a connected session or a completed dump investigation.
- Exercise crash preparation with a fixture manifest and missing-input cases.
  It must preserve reports and must not claim preparation is completed triage.
- Disclose unavailable sessions and sampling/security blockers. Do not change
  debugger protections to make validation pass.
