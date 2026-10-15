# PPR Engineering Contract

This is the authoritative, repository-wide contract for PPR. It defines enduring
architecture and coding rules; named skills own operational procedures. When this
file conflicts with a skill on *how* to perform a specialized task, follow the
skill. The active OMO preset configuration is the sole authority for agent,
skill, command, and MCP permissions—do not duplicate or infer those permissions
here.

## Repository facts

PPR is a high-performance real-time C++23 engine built with C++20 modules.
`game/Game.Application.cpp` hosts the `Application` run loop; `game/main.cpp` calls its exported `runGame` entry point. `include/pP/Macros.h` is the
public macro header.

The dependency graph is directional and must remain acyclic (see `codemap.md`;
on conflict `CMakeLists.txt` wins over this file). `engine.physics` exposes
PUBLIC `core, math` and PRIVATE `box2d::box2d`
(`lib/engine/physics/CMakeLists.txt`); `engine.app` links neither `sim` nor
`physics`; `game` links `sim` plus PRIVATE `physics` for its driver-owned
Scene and chunk colliders (`game/CMakeLists.txt`); the other physics consumer
is `engine.tests.sim` via PRIVATE link (`lib/engine/tests/sim/CMakeLists.txt`).

`engine.core` supplies types, allocators, containers, concurrency, services,
I/O, and HAL. `engine.math` wraps Mango math. `engine.shader` compiles Slang
shaders; `engine.rhi` wraps Slang-RHI; `engine.sim` provides
renderer-independent simulation state (chunk grid, fixed-timestep tick,
snapshot); `engine.app` supplies application, platform, input, window, player,
scene, UI, and renderer integration. HAL platforms are Windows, Linux, Darwin,
and Generic.

`ApplicationDomain` explicitly declares whether an application is headless,
interactive, presence-aware, rendering, and UI-capable; it is immutable after
construction. Client/editor code owns
scene, player, camera, viewport, and UI state; it submits work to the renderer
rather than transferring that ownership to it.

For presets, exclusions, and gates see `codemap.md` and **`build-system`**.
Read the root `codemap.md` before working; read a directory's `codemap.md` for
work in that area.

## Architecture and module boundaries

- Dependencies may only point down the graph above. A lower layer must not
  import, include, resolve, or otherwise depend on a higher layer.
- A new source file and its CMake registration are one change. A `.cppm`
  exports declarations (plus definitions only for exported templates and inline
  functions); non-inline, non-template definitions belong in the matching
  `.cpp`. Follow **`module-architect`** for module procedure and
  **`build-system`** for targets and dependencies.

## Design simplicity and reuse

- Prefer existing engine facilities when their contracts fit. Before adding a
  parallel facility, check the owning subsystem; extend or reuse it unless a
  concrete semantic, dependency, or measured cost mismatch justifies separation.
- Introduce abstractions and services for a current responsibility, required
  boundary, or demonstrated variation — not hypothetical future reuse. Prefer a
  local implementation when indirection adds no present value; keep ownership
  and dependencies explicit.
- Implement the smallest complete solution to the current requirement, including
  required failure and teardown behavior. Do not add speculative extension points,
  configuration, generic frameworks, or fallback paths.
- Keep changes focused. Do not generalize unrelated code or extract shared helpers
  solely because implementations look similar.

## Runtime cost

- Treat CPU/GPU work, allocation, synchronization, and resource growth as design
  constraints. Material hot-path changes require proportionate cost evidence;
  claimed performance improvements require measurement.
- Applicable product budgets are defined outside AGENTS.md; do not invent
  engine-wide thresholds. Do not introduce speculative optimization, caching, or
  concurrency without demonstrated need.
- Preserve useful profiling coverage when adding or changing material runtime
  work. Use `hal::ProfileScope` at meaningful phase, batch, subsystem, and worker
  execution boundaries where timing distinguishes actionable causes of a slowdown.
  Reuse enclosing coverage when it already answers that question.
- Do not instrument every function, trivial forwarding/accessor operations, or
  per-item inner loops by default. Prefer an aggregate scope around the batch;
  add finer scopes for a concrete investigation, considering invocation frequency,
  runtime overhead, and trace volume. Review granularity without a function-coverage
  percentage or universal duration threshold.
- Prefer named RAII scope objects with stable, descriptive literal names. Manual
  `hal::profileBegin`/`hal::profileEnd` pairs must remain balanced and properly nested
  on the same thread across all exits. Do not span asynchronous handoffs or
  coroutine suspension with these thread-local scopes.
- Use `hal::profile_blocking_category` around actual CPU wait operations, keeping
  useful work outside the scope where the API permits. A wait API that also runs
  callbacks measures that combined elapsed time, not pure blocked time. See
  [profiling guidance](docs/profiling.md) for placement and interpretation, and
  **`pix-profiling`** for capture procedures.

## Ownership, lifetime, and teardown

- Make ownership visible in types. RAII values and `unique_ptr` own; references,
  views, callbacks, `function_ref`, and `safe_ptr` do not own.
- `safe_ptr` is a non-owning lifetime-checked pointer in debug and a raw pointer
  in release. The owner must outlive every `safe_ptr`; it is never shared
  ownership. Do not conceal ownership or lifetime in global/singleton state.
- Inject dependencies through constructors, parameters, or explicit service
  boundaries. Resolve services at a top-level boundary, not silently in core
  logic. No hidden singleton may determine an object's lifetime.
- Teardown is the inverse of setup: first detach or disable callbacks,
  listeners, and submissions, and prevent new work; then cancel, wake, drain,
  or join in-flight work; then release paired resources in a valid dependency
  order. Pair every successful acquisition with its corresponding release,
  including partial-initialization paths.
- Shutdown is best effort: attempt all independent cleanup, preserve and return
  the first error, and never hide a cleanup failure behind later work.
- Time is an injected explicit dependency. Use an explicit clock at the
  application boundary; do not let lower-level logic read an implicit clock.
- For allocator selection, poisoning, and arena rules use **`memory-allocator`**;
  for channels, signals, contexts, and async I/O use **`concurrency-patterns`**.

## Errors and operational boundaries

- Use the custom logger and public macros in `include/pP/Macros.h` for engine
  diagnostics; see [logging and error propagation](docs/logging-and-errors.md)
  for category setup, structured fields, filtering, and macro semantics.
- Error-returning functions must use the applicable `PPR_RETURN_ERROR_ON_FAIL`,
  `PPR_RETURN_UNEXPECTED_ON_FAIL`, or `PPR_RETURN_ON_FAIL` macro for ordinary
  failure propagation and immediate error returns. This includes locally
  produced error codes, not only failures returned by callees. For best-effort
  cleanup, use `PPR_RETAIN_ERROR_ON_FAIL` to attempt all independent cleanup and
  preserve the first error, then return the retained code. Explicit branches are
  required when recovery, expected absence, error translation, or cleanup must
  occur before returning; document that handling and propagate the final error
  with the applicable macro. Do not replace these macros with handwritten
  log-and-return checks. `PPR_LOG_WARNING_ON_FAIL` is only for deliberately
  nonfatal failures and does not propagate an error. Success returns remain
  ordinary returns. The header's names end in `_ON_FAIL`; there is no
  `PPR_RETURN_ERROR_IF_FAILED` macro.
- Use `std::error_code` for no-value lifecycle and operational actions:
  filesystem, process, platform/HAL, GPU/shader, service initialization, and
  shutdown. Value-producing recoverable operations use
  `std::expected<T, std::error_code>` or the established equivalent. Propagate
  or translate errors with context; do not convert a recoverable failure into
  an opaque false/null/default result.
- Assertions express violated programmer invariants, not ordinary operational
  failure. `PPR_ASSERT` and `PPR_VERIFY` are not test assertions because their
  release behavior uses assumptions. Tests use `PPR_TEST_ASSERT`.
- Make failure and cancellation visible in the signature or result. A caller
  must be able to distinguish success, expected absence, and failure.
- Never swallow an exception silently to satisfy `noexcept`: a bare
  `try { ... } catch (...) {}` with no log, error code, or diagnostic is
  forbidden. Either remove `noexcept` and let the failure reach a reporting
  boundary, or translate it into the function's `std::error_code` /
  `std::expected` result (`system_error` → its code, otherwise
  `state_not_recoverable`). `std::bad_alloc` is fatal: let it reach the
  application's reporting/fatal boundary, including when it occurs during
  diagnostics. Do not translate it into `not_enough_memory` or swallow it in
  a catch-all handler. Best-effort
  paths (hints, diagnostics, best-effort teardown logging) are the only
  exception, and must say so in a comment. Never guard a smart-pointer
  assignment with a blind catch merely to keep `noexcept`; make the owning
  operation fallible instead.

## Rendering and camera contract

- `Renderer` owns content-free, device-facing frame orchestration and encoding
  of submitted work, including surfaces, queue submission, and presentation. A
  render pass owns its content-specific shader, pipeline, buffers, and pass-local
  resources; it produces and encodes its own draw work.
  Do not move scene/content ownership into `Renderer`.
- Renderer boundary types are camera-free. Passes consume a `CameraSnapshot`
  and must not consult a mutable `Camera` while drawing; viewport and scissor
  travel per-draw on `DrawSubmission`.
- The camera/matrix contract is row-major, row-vector, left-handed, +Z-forward,
  +Y-up, with `[0,1]` depth: use `mul(float4, matrix)`, view-projection
  `view * projection`, and no backend-specific transpose or Y flip. See
  `App.Scene.Camera.cpp` and `engine.rhi` `RHI.cppm`/`RHI.cpp` (projection
  helpers) for the canonical implementation.

## Tests and delivery readiness

Test externally observable behavior, contracts, error paths, and lifetime and
teardown effects; use `PPR_TEST_ASSERT`. Core/sim tests are GLFW-free;
app/asset tiers use GLFW/headless as documented in `codemap.md`. Use
**`unit-test-updater`** for test changes and **`validation`** for the
post-change checklist.

Before implementation, establish the changed behavior, owning subsystem,
applicable error/lifetime constraints, existing facilities to reuse, and the
smallest meaningful verification in the existing task context. Keep this brief
proportionate; routine work needs no separate planning document or design approval.
Resolve consequential architectural uncertainty before committing to an approach.
The validation skill owns execution scope, evidence reuse, and review timing.

- Deliver complete behavior for the supported scope: success, expected absence,
  failure, cancellation where applicable, and safe partial-initialization and
  teardown paths. Do not present placeholders, silent degradation, or unverified
  claims as completed work.
- Applicable correctness, compatibility, resource, and performance gates are
  acceptance criteria. Report what was verified and any remaining limitation;
  explicitly identify any unmet or unrun gate.
- `std::error_code` converts to bool only explicitly: assert `not ec`
  (or `!ec`) for success and `ec == <expected error>` for specific failures —
  never a bare `error_code` as an assert predicate or bool initializer.

## C++ and API rules

The architecture, ownership, error, and test invariants above apply to C++.
Skills own procedures and must not duplicate or compete with these rules.
- Prefer value semantics, RAII, and the simplest suitable standard or established
  repository facility. Encode ownership and meaningful invariants in types; avoid
  manual resource protocols where an existing owning type fits.
- Prefer `constexpr`, `[[nodiscard]]` for meaningful results, and truthful
  `noexcept`. Use only macros from `include/pP/Macros.h` (plus test macros in
  `pP/UnitTest.h`). Accept the weakest useful input (views/spans, fields over
  wallet aggregates; strong types where they prevent misuse). Framework hooks
  (`main`, application hooks, listeners) are thin glue delegating to engine
  logic.
- Use `not`/`and`/`or` (on one line when short, otherwise trailing-operator one-operand-per-line per Boolean expressions); use `const T` and `T *const`/`const T *const` where the
  callee must not reseat; order attributes, inline-control macros, `constexpr`,
  return type, name, parameters, `const`, `noexcept`.

### Data-member layout

- Review the physical layout of every type whose data members you add or
  change. Minimize avoidable internal and trailing padding.
- Order independent members by decreasing required alignment and group
  small members together. Use actual alignment requirements; do not assume
  that `sizeof` determines alignment. Check the resulting object size,
  including trailing padding that contributes to array stride.
- Preserve initialization and destruction dependencies, external layout
  contracts, and declaration-order semantics. Reorder members only when
  those constraints remain satisfied, and update affected initializers
  and consumers.
- Keep natural alignment. Do not introduce packing, extra `alignas`,
  bit-fields, or narrower field types solely to remove padding or silence
  a diagnostic.
- Preserve intentional padding and alignment required by external layout
  contracts or justified cache-line separation to prevent false sharing.
  Base cache-line separation on actual concurrent access patterns; do not
  apply it indiscriminately. Document the reason when it is not apparent
  from the declaration.
- Verify `sizeof`, `alignof`, and relevant member offsets on the affected
  target/configuration for layout-sensitive or heavily replicated types.
  Add static assertions for actual layout contracts, not incidental
  implementation details.
- Keep this review within the changed types and their affected consumers.
  Do not expand it into unrelated layout refactoring.

### Semantic control flow

**Write code in logical paragraphs, separated by one blank line.** A function
with one responsibility can still have several steps; sharing a variable or
contributing to the same result does not make those steps one paragraph. This
applies recursively inside loops, branches, lambdas, and test bodies, including
short bodies. clang-format cannot infer these boundaries; authors MUST insert
them while writing, before handing code to validation or another agent.

- Separate successive steps: establish inputs, prepare working state, traverse
  or calculate, check the result, commit changes, and finalize. A step may be
  one statement (sorting, a propagation macro, or an accumulator update).
- Separate a completed search/count/collection loop from its result check and
  from the next action. Separate rejection/skip guards from accepted-item work,
  and independent sibling cases or traversal passes when each is a distinct step.
- Keep declarations with the step they support, not in one undifferentiated
  setup block. Separate local helper definitions from their use; distinguish
  scratch setup, deferred bookkeeping, and execution when they are separate steps.
- Keep a tightly coupled operation intact: a call and its immediate error
  propagation, a value and its local guard, one object's field initialization,
  one log record's fields, or an update and its terminating `break`/`return`.
  Place guards where their inputs become available; do not move them all to
  the function entry. A final return may stay with the result it completes.
- Separate adjacent function definitions (including short members) and distinct
  declaration groups. In classes, distinguish constants, related state groups,
  construction, mutation, friend declarations, move operations, deleted copy
  operations, and query groups where present. Keep related pairs together.
  Keep documentation comments attached to their declaration, with the separator
  before the comment when starting a new group.

Read each touched body and declaration block as a sequence of steps before
handoff, then check that sequence after formatting. Missing boundaries are an
incomplete edit even if format/style scripts pass. Preserve existing intentional
grouping when editing; dense surrounding code is not a precedent to copy. Do not
use line-count quotas, a blank after every statement/brace, or a token-saving
dense draft for a later agent to repair. A genuinely single-step body needs no
internal blank line. Keep the success path contiguous in control flow; that
does not mean removing its paragraph breaks. Extract helpers for distinct domain
operations or reuse; comments and extraction do not replace paragraph boundaries.

For example, preparation stays together, followed by submission and bookkeeping:

```cpp
DrawSubmission submission{};
submission.m_viewport = viewport;
submission.m_scissor = scissor;

PPR_RETURN_ERROR_ON_FAIL(Render, submit(submission));

++statistics.m_submissions;
statistics.m_vertices += vertex_count;
```

For examples grounded in the authored Layout corrections and commit `6fc03a2`,
see [C++ logical grouping](docs/cpp-logical-grouping.md) when calibrating a boundary.

### Section headers

Authors MUST label the major sections of C++ interfaces, implementations, and
test files with the established three-line header (as in
`App.Input.Listener.cppm` and commit `6fc03a2`):

```cpp
// ------------------------------------------------------------------
// layout compilation
// ------------------------------------------------------------------
```

Use concise responsibility names for cohesive type/API families, implementation
areas, and test suites; include the first substantive section, not only later
transitions. Put the header at the section's indentation, separated from adjacent
content by a blank line, before any attached `///` documentation. Preserve and
extend these headers when adding sections. Inside classes, use a short label
such as `// listeners:` for distinct member/API subgroups needing a name.
Ordinary function-body steps use blank lines; do not put a banner over every
function, guard, or paragraph. A tiny forwarding/umbrella file with no substantive
section needs no artificial header. Headers provide navigation; paragraph breaks
provide local flow. Both are author-owned and independently required.

### Boolean expressions

Any boolean-operator expression — `if`/`while`/`for` conditions, returns, assignments,
assertions — SHOULD stay on one line when it fits, where "fits" means it stays
within about 120 columns, matching the configured `ColumnLimit`. Wrap it only when
it overflows, and then break AFTER the operator, never before it:
- MUST: the operator ends its line; the next operand starts the next line.
- NEVER: a continuation line starting with `and`/`or`.
- NEVER: an operator stranded alone on its line.
- NEVER: `and(`/`or(`/`not(` without a trailing space.
- NEVER: a missing space in a comparison (`x<max`); always `x < max`.
- MUST: keep `not` attached to its operand (`and not x`); never strand `not` at end of line.

## Source format

The repository-root `.clang-format` is the canonical mechanical formatting authority.
The CLion project style (`Project.xml`) is demoted to IDE convenience only and
never overrides it, and the IDE must be configured to format C/C++ through the
external `clang-format` binary rather than its own engine. Mechanical formatting
is applied by script; authors insert semantic blank lines as described above:

- `pwsh -NoProfile -File scripts/Format-Cpp.ps1 -Mode Fix` rewrites the lines the
  working tree changes.
- `pwsh -NoProfile -File scripts/Format-Cpp.ps1` (Check) is the gate. It exits 0
  only when the working tree already holds the canonical output, the case-sensitive
  code token stream (including string and char literal contents) is unchanged, and
  repeated formatting is a fixed point. Run it on touched C++ before finishing.
- `pwsh -NoProfile -File scripts/Check-SemanticStyle.ps1 -Files '<a.cpp,b.cpp>'`
  checks the alternative-token rules on those files. Blocking classes are
  A-STRANDED, B-OP, and C-LEADING; `-Strict` also fails on advisory findings.
  Run it repo-wide only as a deliberate audit, never as a per-change gate.

`ColumnLimit` is `120`, so the formatter wraps and reflows expressions and
normalizes brace placement. It does not determine semantic group boundaries;
author-inserted blank lines remain subject to the configured empty-line limits.
Division of labour:

- **The script owns** mechanical formatting: indentation, spacing, alignment,
  brace placement, expression wrapping, and consecutive empty-line limits.
- **You own** logical paragraphs, declaration groups, section headers, meaningful
  helper boundaries, and comments that state intent or invariants.
- **`code-reviewer` owns** verifying those boundaries and headers in the formatted
  source. Passing mechanical checks does not establish this acceptance criterion.

Do not use the CLion `reformat_file` router or hand-tune mechanical formatting;
run the script after semantic edits. CLion's internal engine does not
consistently apply the project style, reflows `and`/`or`/`not` chains away from the
trailing-operator rule, and injects namespace end-comments; non-idempotent
reformatting is a confirmed upstream defect class. The IDE is configured to
delegate C/C++ formatting to the external `clang-format` binary so the script and
the editor cannot disagree.

Expression wrapping remains script-owned, but the alternative-token rules above
are not fully enforced by the formatter: clang-format re-flows these chains but has no option to
normalise their spacing or operator placement. `scripts/Check-SemanticStyle.ps1` is
that enforcement. Run it on any file where you wrote or changed `and`/`or`/`not`.

## Specialist authorities and workflow

Skills live at `.opencode/skills/<id>/SKILL.md`; use the named skill instead of
reproducing its procedure here:

| Need | Authority |
|---|---|
| Builds, presets, dependencies, sanitizers, diagnostics | `build-system` |
| Modules, exports, partitions, `import std` | `module-architect` |
| Slang shaders, reflection, CPU/GPU layouts, and Slang-RHI bindings | `slang-shader-developer` |
| HAL changes | `hal-developer` |
| Allocators and `safe_ptr` mechanics | `memory-allocator` |
| Concurrency and async I/O | `concurrency-patterns` |
| Test updates | `unit-test-updater` |
| Validation | `validation` |

The `fixer` agent is edit-only (no builds); builds and tests run via the
orchestrator per `build-system`. The `clion` MCP server exposes exactly one
tool, the `execute_tool` router; agents call it with `projectPath="E:/Code/ppr"`.

### Delegated implementation and plan changes

Delegation authorizes implementation within the accepted plan, not unilateral
redesign. This applies to OMO `fixer` and equivalent Codex implementation
subagents, regardless of model. The planning owner is `oracle` in OMO; in other
environments the coordinator identifies the agent responsible for design
decisions. These responsibilities do not grant additional tools or permissions.

- A delegated task carries the accepted plan or relevant excerpt, bounded edit
  scope, facilities to reuse, required behavior, and any already accepted
  fallback. Reuse the existing task context; no separate planning document is
  required. A missing consequential design decision goes back to the coordinator.
- Implementation details and established fixes within that scope remain the
  implementer's responsibility. Replacing a chosen subsystem with bespoke
  functions, changing ownership or dependency boundaries, weakening validation,
  or dropping supported behavior requires a planning-owner decision before edits.
  Passing tests or making a build succeed does not authorize those changes.
- An unexplained compiler ICE or an unexpected feasibility blocker requires
  the implementer to stop affected production edits and return `BLOCKED_PLAN`
  through the coordinator. Do not iterate speculative production workarounds or
  silently substitute another design. Read-only investigation may continue
  within existing grants; builds and reproducer execution stay with their owner.
  A complete diagnosis or minimal reproducer is not a prerequisite for escalation.
- The handoff identifies the blocked plan step, observed failure and available
  command/preset/toolchain/log evidence, attempted changes and their outcomes,
  current diff and any experimental edits, known facts versus hypotheses, missing
  evidence, and the decision needed. Reference existing artifacts; say when
  evidence is unavailable. Preserve user work and report partial state rather
  than performing a broad rollback or presenting a substitute as complete.
- The coordinator routes this evidence to the planning owner for re-evaluation.
  Do not reassign the same blocker with an unrestricted "make it work" goal.
  The planning owner may request targeted diagnosis or accept a bounded remedy
  or revised plan. Record that decision in the task context and return explicit
  scope to the implementer before affected implementation resumes. If that
  decision is unavailable, keep the affected work blocked; independent approved
  work may continue.
- A useful blocked handoff fulfills the delegated reporting duty; the overall
  task remains incomplete. Completion pressure, retries, and tool failures do
  not transfer design authority to the implementation lane.

## Repository Map

Read the root `codemap.md` before working and a directory's `codemap.md` for
work in that area (use the linked maps and current `.codegraph/` index).
