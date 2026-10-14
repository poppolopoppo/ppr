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
dimensions. In-plan implementation defects go to `@fixer`; unexplained compiler
ICEs and plan/feasibility blockers go through the coordinator to `@oracle` under
AGENTS.md's delegated-implementation contract. Only affected checks are repeated.

## Execution flow

1. Before implementation, establish the acceptance and verification scope in the
   existing task context, following AGENTS.md and the test skill's fixture
   preflight. Identify whether this is a local batch, completed behavior/phase,
   or merge/release gate. Honor explicit user scope. Otherwise a local batch
   defaults to the affected Dev target closure and justified focused tests;
   do not expand it to all presets or the full CTest matrix automatically.
2. The author self-reviews behavior, error/lifetime contracts,
   [data-member layout](../../../AGENTS.md#data-member-layout), and the
   [logical paragraphs and section headers](../../../AGENTS.md#semantic-control-flow)
   while implementing, including nested bodies and declaration groups. This is
   part of authoring, not a cleanup task deferred to the formatter or reviewer.
   Consult a specialist before coding only when
   unresolved architectural or domain uncertainty could cause substantial
   rework; routine work needs no separate design approval or planning artifact.
3. Once a coherent batch is ready, run Format Fix then Check and the touched-file
   semantic-style gate before expensive execution. Confirm the author inspected
   the formatted bodies, declaration groups, and section headers against that
   contract. Route missing boundaries/headers to the author/fixer, then
   rerun affected gates. Mechanical formatting is script-owned; semantic blank
   lines and headers are author-owned. Do not format or recheck after every small edit.
4. For completed behavior, use one context-owning independent reviewer under
   `code-reviewer`. Stable-code review may precede or overlap long execution so
   findings arrive early; it need not wait for the last build, test, or CLion
   result. Keep source-review and execution statuses distinct until final
   reconciliation. Re-review fixes as a delta, widening scope only when their
   effects require it. Local edit batches use author self-review, not an
   automatic additional independent review for every iteration.
5. Configure, build, and test the selected scope through `build-system` in the
   persistent shell, with builds and executable checks serialized. Run triggered
   boundary/performance checks once; do not substitute a tiny fixture for a claim
   that requires production parity or stress. Collect the evidence required by
   the data-member layout contract through these checks. Discover current IDE capabilities
   when needed and perform the staged touched-file CLion inspection below when
   available. Report unavailable or timed-out inspection honestly.
6. Reuse passed evidence when the checked source/dependency closure, configuration,
   tools, and workload remain applicable. After a fix, rerun failed and affected
   checks only; a broader suite is repeated only if its evidence was invalidated
   or a delivery/risk trigger requires new coverage. Reconcile final review and
   execution results; unresolved findings or required unrun gates leave delivery
   incomplete. Summarize passed, failed, and skipped checks with reasons.

## Scope rules

- **Local edit batch:** affected Dev build and one loop of justified focused
  leaves/groups, plus applicable format/style checks and author self-review.
  Record why those checks prove the changed claim. Use a whole affected suite
  when focused selection cannot establish it. Additional presets require an
  explicit request or concrete compatibility, optimization, or toolchain risk.
- **Completed behavior/local phase:** one loop of all affected suites and one
  independent review, reusing applicable earlier results. Cross-cutting shared
  app/render-lane transaction changes require the full twelve-entry matrix,
  including asset and both shipped-profile validators; a bounded app/UI edit
  alone does not trigger it. Define completion at a coherent behavior boundary,
  not at every tool call, file edit, or conversational turn.
- **Pre-merge CI:** run the full matrix for one loop plus the platform/build
  matrix from `build-system`.
- **Nightly stress:** run the full ASan matrix with three same-process
  shuffled loops (`-DPPR_TEST_LOOPS=3`).
- **High-risk merge:** run three-loop affected suites for concurrency,
  lifetime, teardown, or render-transaction changes.
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
- Script/tooling changes use syntax checks and focused behavior/exit-code cases;
  they do not trigger C++ builds or CTest unless engine behavior is affected.

## Evidence and efficiency

Use the existing validation record, not a new tracking service. Retain exact
commands, scope/preset, exit status, wall time, log path, and source/configuration
identity sufficient to judge evidence reuse. Record first-build failures, review
fix rounds, repeated checks and their reason, and subsequent reopened defects.
When session usage is available, keep uncached input, cached-input subset and
output tokens distinct. Report concurrent interval unions separately from summed
agent time; do not infer savings, phase costs, or billing from command counts.
Compare similar tasks before claiming that the revised workflow is faster.

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
`.clang-format` is the canonical mechanical formatting authority; the CLion project
style is IDE convenience only. Mechanical formatting goes through one script, which
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
  orchestrator; do not hand-tune mechanical formatting or rerun the formatter hoping
  for a different result.
- Do not use the CLion `reformat_file` router. It does not consistently apply the
  project style, reflows `and`/`or`/`not` chains away from the trailing-operator
  rule, injects namespace end-comments, and inherits a confirmed upstream
  non-idempotency defect class.
- Authors insert semantic blank lines and section headers under AGENTS.md. A
  successful Fix/Check establishes mechanical layout only; it cannot certify
  logical paragraphs or supply missing headers. Do not hand-tune mechanical
  formatting; rerun the gate after semantic grouping edits. Reuse a passed Check
  only while its source and check inputs are unchanged. Fix and Check remain
  separate: convergence during Fix alone does not establish the final Check.
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
  Default output contains blocker details and advisory counts; use
  `-ShowAdvisories` for detailed advisory inspection. Counts do not prove semantic
  readability, and a single arbitrary blank line is not a readability fix.
- Route blocking findings to the orchestrator for a bounded fix, then repeat the
  formatting and style gates.
- Track semantic grouping/section-header review separately from the two script
  exit statuses, using author self-review for local batches and the existing
  independent reviewer at completion. A green script result or absent D-DENSE
  advisory cannot mark that review passed. Apply AGENTS.md's contract through
  `code-reviewer`, without another review pass or a numeric density threshold;
  unresolved confirmed missing boundaries or headers block delivery.

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

- Build/test failure: preserve the command, preset, toolchain, exit code, and
  relevant log. Distinguish an in-plan defect from an unexplained compiler ICE
  or a remedy that would change the accepted design. Route the latter through
  the `BLOCKED_PLAN` handoff in AGENTS.md before production workaround edits;
  the execution owner supplies requested diagnostic/reproducer evidence. After
  an authorized fix, rerun only the affected preset/check.
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
