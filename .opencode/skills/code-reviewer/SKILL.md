---
name: code-reviewer
description: >
  Evidence-backed review of tracked PPR changes, centered on architectural
  boundaries, API/lifetime contracts, behavior, and project conventions.
---

# Code Reviewer

## Contract

Review the task's coherent change scope, including its staged/unstaged changes
and relevant untracked or ignored replacements; do not edit or stage files.
Do not absorb unrelated working-tree changes into a review. This skill owns the
review workflow and finding format; domain skills own their specialized
mechanics. Report only findings supported by the current diff and targeted
source evidence. `@fixer` applies bounded fixes; `@oracle` adjudicates a
disputed finding. The orchestrator owns final validation and the overall
verdict.

## Review process

Use one independent review for completed behavior, not a new reviewer for every
edit batch or dimension. The author self-reviews during implementation. An early
specialist consultation is justified by consequential unresolved design risk;
it is not an automatic second full review. Reuse the existing reviewer for
targeted fix deltas and reopen broader scope only when those deltas affect it.

1. One context-owning reviewer enumerates the supplied scope's review inputs without
   staging anything: `git diff --name-status` (unstaged tracked),
   `git diff --cached --name-status` (staged tracked), and untracked plus ignored
   replacement documentation under `AGENTS.md`, `README.md`, `codemap.md`,
   `.opencode/skills/`, and `.opencode/commands/`. Use
   `git ls-files --others --exclude-standard -- <paths>` and
   `git ls-files --others -i --exclude-standard -- <paths>` for the latter two
   sets. If no scope was supplied, establish it from the current task and changed
   paths before loading diffs. Reuse current scoped diffs, instructions, and
   specialist evidence; refresh only changed inputs. Before relying on `build-system`, check
   `git check-ignore -v --no-index .opencode/skills/build-system/SKILL.md`; if it
   is ignored, report the ignore rule and do not stage it automatically. Then
   gather the relevant diff, changed files, and repository instructions for each
   enumerated input. Reuse an
   existing specialist session only when it already has relevant, current
   context and avoids rediscovery.
2. Classify the change and choose proportional depth: docs/config, bounded code,
   or high-risk cross-system. Do not launch a reviewer per dimension.
   Record only triggered risks and evidence needed; omit per-dimension
   no-findings narratives.
   For delegated implementation, compare the diff with the supplied accepted
   plan and recorded planning-owner decisions. An unapproved design substitution
   is a blocking finding even when tests pass; use the escalation contract in
   AGENTS.md. Request missing decision context rather than assuming approval.
3. Inspect changed code and only the directly relevant callers, owners,
   teardown paths, module interfaces, and tests needed to prove or refute a
   concern. Prefer project-aware search and targeted reads; avoid repeatedly
   loading the full diff or full files.
4. Escalate to a targeted domain specialist or `@oracle` only for an actual
   ambiguity or a high-risk finding needing specialist judgment, and pass the
   already gathered evidence rather than re-running discovery.
5. For every finding, cite `file:line`, the violated contract, the concrete
   consequence, and the smallest credible remediation. Re-check non-IDE
   findings against current source before presenting them.
6. Stable-code review may happen before or alongside long validation. Mark the
   source review **passed**, **blocked**, or **proportionately reviewed**, and
   identify any execution evidence still pending. The orchestrator reconciles
   final evidence; do not launch another full review merely because test logs
   arrived. Confirmed Errors and Warnings block; Suggestions are advisory.

## The seven dimensions

### 1. Module and layer boundaries
- Dependencies preserve the documented direction; lower layers do not import,
  include, or otherwise depend on higher layers.
- Partitions remain focused. Source registration, umbrella re-exports, and
  imports follow `module-architect`.

### 2. Exported surface and public API contracts
- Exports are minimal and deliberate; implementation helpers stay private.
- Public signatures make ownership, mutability, threading, failure, and ordering
  requirements visible. Inputs use the weakest useful type and avoid hidden
   service/global access in leaf logic.
- Check compatibility only when persisted, wire, plugin, or exported contracts
  change.

### 3. Ownership, lifetime, and teardown
- `unique_ptr` and RAII values own. `safe_ptr`, callbacks, references, and views
  are non-owning and cannot outlive their owner.
- Teardown first detaches or disables callbacks, listeners, and submissions and
  prevents new work; then cancels, wakes, drains, or joins in-flight CPU/GPU
  work; then releases dependent resources in reverse setup order, preserving the
  first cleanup error.

### 4. Lifecycle and error semantics
- Initialization, operational boundaries, and shutdown expose meaningful
  success, absence, failure, and cancellation states.
- Recoverable operational failures use and propagate contextual `std::error_code`
  as required. Shutdown attempts independent cleanup and preserves the first
   error rather than hiding it.
- Check capacity, reclamation, and backpressure only when resource-growth policy
  changes.

### 5. Subsystem responsibility and control flow
- For new facilities, check current purpose and reuse of the owning subsystem;
  screen material hot-path changes for cost evidence.
- For changed data-member declarations, enforce
  [AGENTS.md's data-member layout contract](../../../AGENTS.md#data-member-layout).
  Report confirmed avoidable padding with a safe alternative and concrete size
  or stride impact; padding alone is not a violation.
- Each subsystem owns one coherent responsibility. Renderer/pass, scene/camera,
  service, and HAL boundaries retain their documented roles.
- Framework hooks and callbacks are thin glue that delegates to engine logic;
  functions do not mix orchestration with unrelated low-level work.
- Read complete touched function, lambda, loop, branch, and test bodies as a
  sequence of logical paragraphs under
  [AGENTS.md](../../../AGENTS.md#semantic-control-flow), including nested steps.
  One overall responsibility does not imply one paragraph. Check search/count
  versus result validation, rejection versus accepted-item work, independent
  passes/cases, and preparation versus mutation/bookkeeping. Check guards where
  their inputs become available; do not require moving them to function entry.
  Contiguous fall-through describes control flow, not the absence of blank lines.
  Continue checking excessive nesting and repeated early exits that obscure the
  successful path.
- A semantic separation finding must identify the adjacent responsibilities and
  the missing or misplaced blank-line boundary. Clear violations of AGENTS.md
  are blocking Warnings even when formatting passes; neither function length nor
  statement counts establish a violation. Keep related instructions together
  and do not demand a blank line after every statement, guard, or control block.
  Named helpers should express a distinct domain operation or reuse rather than
  an arbitrary line-count threshold, and side effects and submissions stay in
  the caller. Structured logging fields remain one logical block and do not
  justify flattening surrounding control flow. Use the
  [authored examples](../../../docs/cpp-logical-grouping.md) when a boundary needs
  calibration; do not demand a new helper or explanatory comment instead of a
  missing blank line.
- Reuse the lexical tripwire's blocker result; do not promote heuristic
  advisories directly into findings. When density is a concern, inspect the
  relevant bodies or request scoped `-ShowAdvisories` output. Resolve a D-DENSE
  advisory as either a coherent group or a specific semantic separation issue;
  an advisory alone is not a violation and its absence does not prove readability.

Trigger cost review when change alters recurring per-frame, per-tick, per-item,
submission, or streaming work via iteration count, algorithm, allocation, sync,
transfer volume, shader work, or retained resources. Renames/comments/equivalent
extraction do not trigger measurement.

For material runtime changes, review profiling coverage under
[AGENTS.md](../../../AGENTS.md#runtime-cost) and the
[placement guide](../../../docs/profiling.md). Check whether meaningful phases,
batch/worker execution, and waits remain distinguishable. Identify redundant
nesting, per-item event multiplication, dynamic-label costs, unbalanced manual
pairs, or scopes spanning threads/suspension. `Blocking` labels wait operations;
mixed native waits may include callbacks and are not pure blocked time. A missing
marker alone is not a finding: identify the diagnostic question left unanswered
and the smallest useful boundary. Do not impose per-function coverage or numeric
granularity thresholds. Reuse validation's trace/cost evidence; flag unavailable
evidence without treating successful capture or no-fault tests as sufficient.

#### Application and rendering boundary checks
- A platform/application client owns application-facing window, viewport, and
  scene coordination; device-facing renderer code does not acquire or retain
  that client or its mutable state.
- Renderer orchestration remains content-free and device-facing. A content pass
  owns its shader, pipeline, buffers, and draw encoding rather than moving that
  responsibility into `Renderer`.
- Submitted rendering consumes a `CameraSnapshot`; viewport and scissor travel
  per-draw on `DrawSubmission`/`DrawContext`, and draw code must not read a
  mutable `Camera` during encoding.
- `ApplicationDomain` is immutable after construction; application-boundary
  changes must not mutate it to alter runtime capabilities.

### 6. Behavioral and domain tests
- Observable behavior changes have focused tests at the appropriate core/app
  boundary, including meaningful failure, lifecycle, or teardown effects.
- Tests use the project test contract (`PPR_TEST_ASSERT`) and validate behavior,
   not private implementation structure.
- Require focused cost, compatibility, or operational evidence only for triggered
  concerns; reuse validation evidence rather than duplicating execution.

### 7. Project formatting and local conventions
- Changes follow the repository-root `.clang-format` as enforced by
  `scripts/Format-Cpp.ps1`, plus `AGENTS.md`, naming, module split, macro, and
  comment conventions. The mechanical gates own indentation, spacing, and
  alignment and expression wrapping; this review owns logical paragraph and
  declaration boundaries and [section headers](../../../AGENTS.md#section-headers).
- Inspect adjacent definitions, class state/API groups, and attached declaration
  comments as well as bodies. Verify the named major sections in interfaces,
  implementations, and test suites, including the first substantive section.
  Missing required headers are blocking Warnings: identify the unlabeled section
  and its scope. Blank lines, braces, and `public:` alone do not name a section;
  a banner does not excuse missing paragraphs inside it. Do not require a banner
  per function or extend review to unrelated source merely to restyle it.

## Proportional gates

- **Docs-only:** verify factual claims, links, referenced skill/command names,
  and scope. Do not demand code-only review or builds.
- **Config-only:** verify syntax and real target/path names, then request the
  smallest affected configure/build/test evidence when behavior can change.
- **Bounded code/test change:** inspect only the relevant dimensions plus the
  immediate callers, ownership paths, and behavioral tests. Use validation for
  execution evidence rather than inventing a second validation workflow.
- **High-risk cross-system change:** inspect all relevant dimensions and trace
  affected module boundaries, lifecycle/teardown paths, and integration tests;
  escalate only unresolved specialist questions.

## Finding format

```
<severity> — <dimension>
<file:line>
Contract: <rule or boundary>
Evidence: <observed code path>
Impact: <concrete failure or maintenance risk>
Remedy: <smallest credible change>
```
