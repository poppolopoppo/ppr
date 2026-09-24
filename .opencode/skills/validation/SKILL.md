---
name: validation
description: >
  Executes proportionate post-change configure, build, test, and inspection
  checks for the PPR engine, then incorporates the code-reviewer verdict.
---

# Validation

## Contract

Validation is execution-focused and orchestrator-owned. It does not edit the
working tree. It selects and runs the requested checks, records exact results,
and treats the `code-reviewer` verdict as a release gate rather than duplicating
review dimensions. Failures are routed to `@fixer` or `@oracle`, then only
affected checks are repeated.

## Execution flow

1. Determine scope and platform. Honor explicitly requested presets/targets;
   otherwise use the standard platform matrix from `build-system`.
2. Configure selected presets sequentially to avoid shared dependency-cache
   races: `cmake --preset <preset>`.
3. Build the full project for each selected preset in parallel workers:
   `cmake --build out/build/<preset>`.
4. Run `engine.tests.core --shuffle` and `engine.tests.app --shuffle` from the
   corresponding build directory, or use
   `ctest --test-dir out/build/<preset> --output-on-failure` when CTest is the
   requested runner. This repository has no CTest test presets.
5. Run targeted CLion inspections for changed code/config files when the IDE is
   available. Report unavailable inspections as skipped, never passed.
6. Invoke `code-reviewer` and incorporate its verdict and findings without
   repeating its taxonomy. A failed or unresolved finding leaves validation
   incomplete even when commands pass.
7. Summarize each command/check as passed, failed, or skipped with its reason.

## Scope rules

- Source, module, build, or runtime-behavior changes normally require the full
  selected preset build and relevant engine suites.
- A focused target/test run is permitted only when the user explicitly scopes
  validation or the orchestrator records why it proves the affected claim.
- Documentation-only changes need no build or executable tests unless they
  alter executable instructions; run proportionate reference/link checks and
  request `code-reviewer`'s docs-only gate.
- Config-only changes run syntax/reference checks plus the smallest affected
  configure/build/test command; expand only if they alter wider behavior.

## WinDbg-tool gate (host-native wiring)

Applies only to windbg-tool wiring changes (`install.ps1`,
`local-services/launch.ps1`, `local-services/uninstall.ps1`, `opencode.json`,
`local-services/README.md`, `.opencode/skills/windbg-triage/`, `.gitignore`
triage block). Otherwise record `windbg_gate: not_applicable` and skip.

- Static (fail when wrong): `install.ps1` parses and references
  `Microsoft.DotNet.SDK.10` with a `dotnet --version >= 10` gate;
  `launch.ps1` parses and defines `Ensure-WindbgTool` / `Verify-WindbgTool`
  (`discover`, `daemon ensure`, `--compact --envelope discover` retry,
  `windbg_tool_*` records); `uninstall.ps1` parses with a best-effort guarded
  `daemon stop`; `opencode.json` parses with a `windbg-ttd` local
  `["windbg-tool", "mcp"]` entry; the `windbg-triage` skill and README
  host-exec section exist; `.gitignore` covers `*.dmp`, `*.run/`, `*.ttd/`.
- Live (Windows host only): `dotnet --version` major >= 10;
  `windbg-tool --compact --envelope discover` exits 0;
  `windbg-tool daemon ensure` healthy. `trace record` probes require inline
  `sudo windbg-tool trace record ...` (New-Window sudo is rejected).
- Skip-vs-fail: fail on any file/expectation mismatch above; warn-and-skip the
  live probe when the tool is absent on non-Windows/CI, recording
  `windbg_gate_live: skipped (<reason>)`.
- Redaction: never log dump/trace contents, only statuses and exit codes.

## Triage

- Build/test failure: preserve the command, exit code, and useful error excerpt;
  fix, then rerun that preset/check.
- Inspection issue: confirm against compiler/build configuration before editing;
  record any unavailable IDE check as skipped.
- Review finding: follow the `code-reviewer` resolution policy. Do not create a
  second review checklist here.

## Report

Report the selected scope, configure/build/test/inspection results, reviewer
verdict, failures or skips, and rerun evidence. Do not claim a green result for
checks that were not executed.
