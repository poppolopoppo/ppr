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
`physics`; `game` links `sim`, not `physics` (`game/CMakeLists.txt`); the sole
physics consumer is `engine.tests.sim` via PRIVATE link (`lib/engine/tests/sim/CMakeLists.txt`).

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

## Tests

Test externally observable behavior, contracts, error paths, and lifetime and
teardown effects; use `PPR_TEST_ASSERT`. Core/sim tests are GLFW-free;
app/asset tiers use GLFW/headless as documented in `codemap.md`. Use
**`unit-test-updater`** for test changes and **`validation`** for the
post-change checklist.

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
Mechanical reformat_file does not satisfy this pass: it only handles indentation, wrapping, spacing,
and braces — it never inserts blank lines, never rebreaks and/or/not chains, and never spaces
alternative tokens (and(, or(, x<). The semantic pass owns all three.
Multi-line conditions use trailing operators with one operand per line: the operator ends
its line and the next operand starts the next line. Never start a continuation line with `and`/`or`,
never leave an operator stranded alone on its line, and never write `and(`/`or(` without a trailing space.

## Source format

The active project CLion C/C++ Code Style (`Project.xml`) is canonical for
mechanical formatting; the repository-root `.clang-format` is a tracked
reference/configuration only and `Project.xml` wins on conflict. Reformat every
touched C++ file after functional writes via `execute_tool` with
`command="reformat_file --files '["E:/Code/ppr/<project-relative path>"]'"`
and `projectPath="E:/Code/ppr"`.

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
work in that area (41 codemaps indexed by `.codegraph/`).
