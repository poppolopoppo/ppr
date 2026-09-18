---
name: code-reviewer
description: >
  Evidence-backed review of tracked PPR changes, centered on architectural
  boundaries, API/lifetime contracts, behavior, and project conventions.
---

# Code Reviewer

## Contract

Review tracked staged and unstaged changes plus untracked or ignored replacement
documentation; do not edit or stage files. This skill owns the
review workflow and finding format; domain skills own their specialized
mechanics. Report only findings supported by the current diff and targeted
source evidence. `@fixer` applies bounded fixes; `@oracle` adjudicates a
disputed finding. The orchestrator owns final validation and the overall
verdict.

## Review process

1. One context-owning reviewer first enumerates all review inputs without
   staging anything: `git diff --name-status` (unstaged tracked),
   `git diff --cached --name-status` (staged tracked), and untracked plus ignored
   replacement documentation under `AGENTS.md`, `README.md`, `codemap.md`,
   `.opencode/skills/`, and `.opencode/commands/`. Use
   `git ls-files --others --exclude-standard -- <paths>` and
   `git ls-files --others -i --exclude-standard -- <paths>` for the latter two
   sets. Before relying on `build-system`, check
   `git check-ignore -v --no-index .opencode/skills/build-system/SKILL.md`; if it
   is ignored, report the ignore rule and do not stage it automatically. Then
   gather the relevant diff, changed files, and repository instructions for each
   enumerated input. Reuse an
   existing specialist session only when it already has relevant, current
   context and avoids rediscovery.
2. Classify the change and choose proportional depth: docs/config, bounded code,
   or high-risk cross-system. Do not launch a reviewer per dimension.
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
6. Mark the review **passed**, **blocked**, or **proportionately reviewed**.
   Confirmed Errors and Warnings block; Suggestions are advisory.

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

### 5. Subsystem responsibility and control flow
- Each subsystem owns one coherent responsibility. Renderer/pass, scene/camera,
  service, and HAL boundaries retain their documented roles.
- Framework hooks and callbacks are thin glue that delegates to engine logic;
  functions do not mix orchestration with unrelated low-level work.

#### Application and rendering boundary checks
- A platform/application client owns application-facing window, viewport, and
  scene coordination; device-facing renderer code does not acquire or retain
  that client or its mutable state.
- Renderer orchestration remains content-free and device-facing. A content pass
  owns its shader, pipeline, buffers, and draw encoding rather than moving that
  responsibility into `Renderer`.
- Submitted rendering consumes a `SceneView`/`CameraSnapshot` paired with a
  `RenderView`; draw code must not read a mutable `Camera` during encoding.
- `ApplicationDomain` is immutable after construction; application-boundary
  changes must not mutate it to alter runtime capabilities.

### 6. Behavioral and domain tests
- Observable behavior changes have focused tests at the appropriate core/app
  boundary, including meaningful failure, lifecycle, or teardown effects.
- Tests use the project test contract (`PPR_TEST_ASSERT`) and validate behavior,
  not private implementation structure.

### 7. Project formatting and local conventions
- Changes follow the active project CLion C/C++ Code Style, `AGENTS.md`, naming, module split, macro, and
  comment conventions. Flag only material inconsistencies, not unrelated style
  preferences.

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
