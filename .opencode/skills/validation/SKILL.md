---
name: validation
description: >
  Executes proportionate post-change configure, build, test, and inspection
  checks for the PPR engine, then incorporates the code-reviewer verdict.
---

# Validation

## Contract

Validation is execution-focused and orchestrator-owned. Apart from the required
`scripts/Format-Cpp.ps1 -Mode Fix` pass, it does not edit the working tree. It
selects and runs the requested checks, records exact results, and treats the
`code-reviewer` verdict as a release gate rather than duplicating review
dimensions. Failures are routed to `@fixer` or `@oracle`, then only affected
checks are repeated.

## Execution flow

1. Determine scope and platform. Honor explicitly requested presets/targets;
   otherwise use the standard platform matrix from `build-system`.
   Record change class and triggered risks in one short line; the scope rules
   below remain authoritative.
2. Run `pwsh -NoProfile -File scripts/Format-Cpp.ps1 -Mode Fix` once over the
   change set, then `pwsh -NoProfile -File scripts/Format-Cpp.ps1` (Check),
   before builds or other validation checks. Formatting is never applied per file
   by hand and never through the CLion `reformat_file` router.
   Reuse a valid Check result when source and check inputs are unchanged; do not
   re-execute an identical check.
3. Configure, build, and test through `build-system`: its platform environment,
   target/preset selection, persistent-shell execution, and batching rules are
   authoritative. Discover current IDE capabilities when needed; do not infer
   them from a historical tool count. Run configurations can implicitly build
   and must refer to executable targets.
4. Run the deterministic gates for touched C++ and block validation on any
   failure: `scripts/Format-Cpp.ps1` (Check) and
   `scripts/Check-SemanticStyle.ps1 -Files '<touched>'`. Reuse the unchanged-source
   Format Check result from step 2; still run the semantic style gate.
   Design-level readability — phase separation, guard grouping, helper extraction
   — is `code-reviewer`'s verdict rather than a formatting check, and is not
   re-litigated here.
5. Run the staged CLion inspection workflow for touched C++ files when the IDE
   is available (tool access follows the `AGENTS.md` guard; procedure is
   below). A final inspection timeout must be disclosed as
   inspection-unavailable/skipped with timeout evidence, never silently
   skipped and never reported as passed.
6. Before the reviewer verdict, run any triggered focused measurement or boundary
   test not already covered. Reuse valid evidence for unchanged code,
   configuration, and workload. For build-performance triggers, use
   `build-system`'s Build-performance workflow and the gate below.
   Invoke `code-reviewer` and incorporate its verdict and findings without
   repeating its taxonomy. A failed or unresolved finding leaves validation
   incomplete even when commands pass.
7. Summarize each command/check as passed, failed, or skipped with its reason.

## Scope rules

- **Local edit batch:** build affected targets and dependents; run one shuffled
  loop of affected suites or justified focused groups; apply format/style
  gates and review. Record why focused groups prove the affected claim.
- **Local phase completion:** run one loop of all affected suites.
  Cross-cutting app/render-lane changes, including Phase 4, require the full
  ten-entry matrix, including asset.
- **Pre-merge CI:** run the full matrix for one loop plus the platform/build
  matrix from `build-system`.
- **Nightly stress:** run the full ASan matrix with three same-process
  shuffled loops (`-DPPR_TEST_LOOPS=3`).
- **High-risk merge:** run three-loop affected suites for concurrency,
  lifetime, teardown, or render-transaction changes; Phase 4 qualifies.
- Asset is not nightly-only: it is required when rendering, surfaces,
  resource lifetime, asset handling, or shared dependencies change. Unrelated
  config-only iterations need not rerun it.
- Keep failure seeds in the validation record; do not use automatic retries
  to mask flakes. Follow `build-system` for loop-cache restoration and
  exclusive matrix scheduling.
- Documentation-only changes need no build or executable tests unless they
  alter executable instructions; run proportionate reference/link checks and
  request `code-reviewer`'s docs-only gate.
- Config-only changes run syntax/reference checks plus the smallest affected
  configure/build/test command; expand only if they alter wider behavior.

## Triggered build-performance gate

For triggers defined in `build-system`'s Build-performance workflow, retain the
representative workload evidence. For optimization claims compare baseline and
candidate with identical workload, toolchain, parallelism, and cache policy.
Repeat paired runs and report samples plus spread, not one percentage. Use
UNINSTRUMENTED timing for the improvement claim; tracing explains it. Preserve
correctness tests, module dependencies, sanitizers, and requested output semantics.
Do not invent a global percentage threshold. Missing required evidence leaves
the claim unverified; a successful capture alone does not prove a fix.

## C++ formatting gate

Applies whenever the change set includes a C++ file. The repository-root
`.clang-format` is the single canonical formatting authority; the CLion project
style is IDE convenience only. All formatting goes through one script, which
applies clang-format to changed lines only and enforces three invariants: the
ASCII code token stream is unchanged, the result is a fixed point within a
bounded pass budget, and in Check mode the working tree already holds the
canonical output.

- Fix, once over the change set:
  `pwsh -NoProfile -File scripts/Format-Cpp.ps1 -Mode Fix`
- Gate:
  `pwsh -NoProfile -File scripts/Format-Cpp.ps1` (or `-Base <merge-base>` in CI)
- Exit 0 is required. `DRIFT` means lines are not canonical, `UNSAFE` means the
  formatter would change the code token stream, and `UNSTABLE` means no fixed
  point within the pass budget. Each class blocks validation and is routed to the
  orchestrator; do not hand-fix whitespace and do not re-run the formatter hoping
  for a different result.
- Do not use the CLion `reformat_file` router. It does not consistently apply the
  project style, reflows `and`/`or`/`not` chains away from the trailing-operator
  rule, injects namespace end-comments, and inherits a confirmed upstream
  non-idempotency defect class.
- Do not hand-edit whitespace to satisfy a layout rule. The script owns layout.
- If no C++ files changed, record this gate as not applicable.

## C++ semantic style gate

Applies to touched C++ after the formatting gate and before CLion inspection.
This gate is deterministic only. Design-level readability is not, and lives with
`code-reviewer`; validation does not duplicate that pass.

- Run the tripwire on the touched files only, never repo-wide:
  `pwsh -NoProfile -File scripts/Check-SemanticStyle.ps1 -Files '<a.cpp,b.cpp>'`.
  `pwsh -File` cannot pass a real array, so pass one comma/semicolon-joined
  argument. An unresolvable path exits 2 rather than reporting a false clean.
- Blocking classes are A-STRANDED, B-OP, and C-LEADING. B-CMP, C-DETACHED, and
  D-DENSE are advisory: B-CMP cannot separate template brackets such as `view<T>`
  from a genuine `x<max`, C-DETACHED brace placement is owned by the formatter,
  and D-DENSE is a blank-line count heuristic that must not become a blocking
  numeric line limit. Exit 1 on blocking findings, exit 0 on advisory-only.
- Add `-Strict` only for a deliberate full audit; it also fails on advisory hits.
- Route blocking findings to the orchestrator for a bounded fix, then repeat the
  formatting and style gates.
- For the design-level pass, expect validation guards grouped at the start of the
  relevant helper, successful work through a contiguous fall-through path, and
  blank lines separating logical phases without fragmenting related declarations,
  guards, or structured logging fields. Extract helpers only for a distinct domain
  operation or reuse, keep side effects and submissions in the caller, and never
  invent numeric line limits. These are review expectations, not gate criteria.

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
  An initial fast `errorsOnly=true` pass after open/readiness is optional; omit
  it when edits and formatting are complete. After all edits and
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
For triggered build-performance work, reference the evidence directory and give
the measurement verdict, explicitly separating measured and pending evidence and
workflow success from any unverified optimization/fix claim.
Use commands and statuses, concise excerpts, evidence references, and one verdict;
do not duplicate design review.
