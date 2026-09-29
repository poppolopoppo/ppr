---
name: validation
description: >
  Executes proportionate post-change configure, build, test, and inspection
  checks for the PPR engine, then incorporates the code-reviewer verdict.
---

# Validation

## Contract

Validation is execution-focused and orchestrator-owned. Apart from the required
CLion reformat gate, it does not edit the working tree. It selects and runs the
requested checks, records exact results, and treats the `code-reviewer` verdict
as a release gate rather than duplicating review dimensions. Failures are routed
to `@fixer` or `@oracle`, then only affected checks are repeated.

## Execution flow

1. Determine scope and platform. Honor explicitly requested presets/targets;
   otherwise use the standard platform matrix from `build-system`.
2. Enumerate changed C++ files and run `reformat_file` via the single
   `execute_tool` router on every one of them before builds or other
   validation checks.
3. Configure, build, and test through the `build-system` authority, not from
   this skill: target/preset/test selection, command form, ordering, and
   runner rules all come from the `build-system` table. Execution is
   cmake-direct inside the one persistent Insiders `vcvars64` shell owned by
   the builder lane — `cmake --preset`, `cmake --build ... --target`, and
   `ctest` all run in that same long-lived shell (`build-system` §0), so
   vcvars is paid once and `VSINSTALLDIR` reaches every configure. There is no
   `build_project`: the live `clion_execute_tool` registry probe (2026-09-28)
   returned 47 tools with no `build_project`, and no router invocation can
   reach it. The orchestrator/builder lane owns project-wide builds (there is
   no per-target IDE selector) and batches them rather than building per edit.
   - A fresh per-invocation shell that skips Insiders `vcvars64.bat` is NEVER
     a build path — it silently resolves Community 14.44 plus a bad vcpkg
     path, and an ad-hoc terminal stays separate from the
     `build-system`-owned persistent-shell route above.
   - `clion_execute_run_configuration` is documented run-only, but it
     implicitly builds the configured run target first (IJPL-217679 /
     IJPL-218400); it is restricted to allow-listed executables, never
     libraries.
4. Perform the post-format semantic readability check for touched C++ before
   inspection, and block validation on any material finding.
5. Run the staged CLion inspection workflow for touched C++ files when the IDE
   is available (tool access follows the `AGENTS.md` guard; procedure is
   below). A final inspection timeout must be disclosed as
   inspection-unavailable/skipped with timeout evidence, never silently
   skipped and never reported as passed.
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

## C++ formatting gate

Applies whenever the change set includes a C++ file. The active project CLion
C/C++ Code Style is canonical for mechanical formatting and is applied through
`reformat_file` via the single `execute_tool` router. The repository-root
`.clang-format` is a tracked reference/configuration only; it is not the agent
formatting authority. Direct
`clang-format`, `git-clang-format`, `clang-format --lines`, and native/manual
whitespace alternatives are not the normal formatter path and must not replace
this gate.

- Enumerate every changed C++ file and call
  `execute_tool(command="reformat_file --files
  '["E:/Code/ppr/<project-relative path>"]'", projectPath="E:/Code/ppr")`
  on each one. Every call MUST pass `projectPath="E:/Code/ppr"`, and the
  `--files` arg MUST be a single-quoted JSON array string.
- Require a successful result for every file. A failed, timed-out, or unknown
  reformat result blocks validation: diagnose and retry the CLion operation,
  and never substitute manual formatting.
- After the final reformat, perform only read-only verification: inspect the
  final content and diff, rejecting any whitespace-only hunks outside the
  functional edit. Do not issue text edits after the final reformat.
- If no C++ files changed, record this gate as not applicable.

## C++ semantic readability gate

Applies to touched C++ after the final CLion reformat and before CLion inspection.
Mechanical reformatting only handles indentation, wrapping, spacing, and braces;
it does not satisfy this gate.

- Re-read the touched control flow for functions that mix setup, validation,
  nested iteration, material/resource resolution, submission, counters, and
  logging, and for deeply nested control flow or repeated early exits that obscure
  the successful path.
- Record mixed responsibilities or unreadable nesting as a material finding and
  block validation until it is corrected. Do not invent numeric line limits or
  flag ordinary personal blank-line preferences.
- Expect validation guards to remain grouped at the start of the relevant helper,
  successful work to continue through a contiguous fall-through path, and blank
  lines to separate logical phases without fragmenting related declarations,
  guards, or structured logging fields.
- Extract helpers only for a distinct domain operation or reuse, keep side effects
  and submissions in the caller, and do not create helpers solely to meet a line
  count. Validation does not edit; route findings to the orchestrator for a
  bounded fix, then repeat the affected formatting and readability gates.
- Run the semantic-style tripwire on touched C++ (`pwsh -NoProfile -File
  scripts/Check-SemanticStyle.ps1`, optionally `-PathFilter '<subdir>'`) after the
  final reformat. Classes A-STRANDED, B-OP, C-LEADING, C-DETACHED, and D-DENSE block
  validation; B-CMP is advisory (template brackets such as `view<T>` are correct,
  genuine `x<max` comparisons are not — eyeball each hit). Route findings to the
  orchestrator for a bounded fix, then repeat the affected formatting and readability gates.

## CLion inspection gate

Applies to touched C++ files when CLion is available. Tool access follows the
`AGENTS.md` guard and the CLion facts recorded in `build-system`; this section
states the procedure and how those results enter the validation record.

- Before every `get_file_problems` router call, open the exact target file with
  `execute_tool(command="open_file_in_editor ...")`. Inspect files
  sequentially; never batch or run inspections concurrently. Every
  `execute_tool` call (e.g. `command="open_file_in_editor ..."` /
  `command="get_file_problems ..."`) must retain
  `projectPath="E:/Code/ppr"`.
- Wait for CLion indexing, project-model, and resolve-configuration activity to
  settle. There is no MCP index-ready endpoint; the IDE indexing indicator is
  authoritative. Do not inspect during active scans or model updates.
- Use a bounded timeout and exactly one attempt per inspection mode per file.
  Run one fast `errorsOnly=true` pass after open/readiness. After all edits and
  formatting are complete, run one final `errorsOnly=false` pass per touched C++
  file. The final warnings-inclusive result is authoritative.
- `timedOut=true` means inspection unavailable, not clean. Do not immediately
  retry. After readiness settles, perform the single final pass. If analyzer
  exceptions or instability recur, restart CLion, wait for the post-restart scan
  to settle, and use the one permitted final retry; do not loop or add more
  attempts. Reindex or invalidate caches only if restart does not stabilize the
  model.
- If the final inclusive pass still times out, record the file as
  inspection-unavailable/skipped, include the timeout evidence, and do not
  claim it passed. Do not automatically substitute direct clang-tidy.
- CLion/JetBrains/ReSharper remains the primary inspection gate. Direct
  clang-tidy does not replace it here because module jobs fail with the current
  MSVC/C++23 module setup. Do not disable the broad profile or clang-tidy first;
  profile reduction is only a later fallback for clean, settled inspections that
  remain slow.

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
  apply the staged CLion timeout-recovery sequence and record every final timeout
  as inspection-unavailable/skipped with the timeout evidence.
- Review finding: follow the `code-reviewer` resolution policy. Do not create a
  second review checklist here.

## Report

Report the selected scope, configure/build/test/inspection results, reviewer
verdict, failures or skips, and rerun evidence. Include final CLion inspection
timeouts in the inspection-unavailable/skipped result with their evidence. Do
not claim a green result for checks that were not executed.
