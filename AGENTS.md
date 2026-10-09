# PPR Engineering Contract

This is the authoritative, repository-wide contract for PPR. It defines enduring
architecture and coding rules; named skills own operational procedures. When this
file conflicts with a skill on *how* to perform a specialized task, follow the
skill. The active OMO preset configuration is the sole authority for agent,
skill, command, and MCP permissions—do not duplicate or infer those permissions
here.

## Repository facts

PPR is a high-performance real-time C++23 engine built with C++20 modules.
`game/main.cpp` hosts an `Application` run loop. `include/pP/Macros.h` is the
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
  `std::expected` result (bad allocation → `not_enough_memory`,
  `system_error` → its code, otherwise `state_not_recoverable`). Best-effort
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

AGENTS.md states the only C++ invariants; skills state procedures and must not
restate or compete with them.

- Dependencies point down the graph only; a lower layer never depends on a
  higher one.
- A `.cppm` exports declarations (plus definitions only for exported templates
  and inline functions); non-inline, non-template definitions belong in the
  matching `.cpp`.
- Make ownership visible in types: RAII values and `unique_ptr` own;
  references, views, callbacks, `function_ref`, and `safe_ptr` do not. The
  owner outlives every `safe_ptr`; never hide ownership or lifetime in
  global/singleton state.
- Inject dependencies (time, I/O, RNG, services, mutable state) at high-level
  boundaries through constructors, parameters, or explicit service boundaries;
  lower-level logic never reads an implicit clock or resolves a hidden
  singleton. Teardown is the inverse of setup (detach, drain/join, release in
  dependency order); shutdown is best effort preserving the first error.
- Use `std::error_code` for no-value lifecycle/operational actions and
  `std::expected<T, std::error_code>` (or established equivalent) for
  value-producing recoverable operations; make failure/cancellation visible,
  never opaque false/null/default. Assertions express violated programmer
  invariants (`PPR_ASSERT`/`PPR_VERIFY`); tests use `PPR_TEST_ASSERT`.
- Prefer value semantics, RAII, and the simplest suitable standard or established
  repository facility. Encode ownership and meaningful invariants in types; avoid
  manual resource protocols where an existing owning type fits.
- Prefer `constexpr`, `[[nodiscard]]` for meaningful results, and truthful
  `noexcept`. Use only macros from `include/pP/Macros.h` (plus test macros in
  `pP/UnitTest.h`). Accept the weakest useful input (views/spans, fields over
  wallet aggregates; strong types where they prevent misuse). Framework hooks
  (`main`, application hooks, listeners) are thin glue delegating to engine
  logic.
- Use `not`/`and`/`or` (on one line when short, otherwise trailing-operator one-operand-per-line per Semantic control flow); use `const T` and `T *const`/`const T *const` where the
  callee must not reseat; order attributes, inline-control macros, `constexpr`,
  return type, name, parameters, `const`, `noexcept`.

### Semantic control flow

After mechanical formatting, do one semantic readability pass on touched C++:
keep validation guards together, the success path contiguous, phases separated,
and distinct domain operations in named helpers.
Separate distinct logical phases (setup, validation, iteration, submission, logging) with blank lines;
do not put a blank line between every statement.
Some parts of this pass are mechanical and some are not, and the difference is not
"what the formatter can do". `scripts/Format-Cpp.ps1` owns every layout decision,
including line breaks (see Source format). You own the logical decomposition into
named helpers and phases, and `code-reviewer` owns whether the result is readable.
Telling "setup" from "submission" is design review, not formatting.
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

The repository-root `.clang-format` is the single canonical formatting authority.
The CLion project style (`Project.xml`) is demoted to IDE convenience only and
never overrides it, and the IDE must be configured to format C/C++ through the
external `clang-format` binary rather than its own engine. Formatting is applied
by script, never by hand:

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

`ColumnLimit` is `120`, so the formatter performs real column-driven wrapping: it
joins and splits statements, and it re-flows them. It does not preserve the line
breaks you wrote — structural rules normalize them (collapsing
`void f() { foo(); }` onto one line, joining a detached opening brace) and the
column limit then decides the rest. Division of labour:

- **The script owns** every layout decision: indentation, spacing, alignment,
  blank-line limits, and line wrapping and breaking. Do not hand-tune any of
  them, and do not hand-adjust whitespace to satisfy a layout rule.
- **You own** the logical decomposition of a statement into named helpers and
  phases, comments that state intent or invariants, and whether the result is
  readable. Line breaks are not yours to choose.
- **`code-reviewer` owns** whether the result reads clearly.

Do not use the CLion `reformat_file` router, and do not hand-adjust whitespace to
satisfy a layout rule either — run the script. CLion's internal engine does not
consistently apply the project style, reflows `and`/`or`/`not` chains away from the
trailing-operator rule, and injects namespace end-comments; non-idempotent
reformatting is a confirmed upstream defect class. The IDE is configured to
delegate C/C++ formatting to the external `clang-format` binary so the script and
the editor cannot disagree.

Because the script owns line breaks, the alternative-token rules above are not
enforced by the formatter: clang-format re-flows these chains but has no option to
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

## Repository Map

Read the root `codemap.md` before working and a directory's `codemap.md` for
work in that area (use the linked maps and current `.codegraph/` index).
