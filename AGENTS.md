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
public macro header. CMake presets include `msvc-dev`, `msvc-live`, `msvc-rel`,
`clang-cl-*`, `clang-*`, and hidden non-module `gcc-*` variants.

The dependency graph is directional and must remain acyclic:

```
app.game -> engine.app
engine.app -> engine.core, engine.math, engine.shader, engine.rhi
engine.rhi -> engine.core, engine.math, engine.shader
engine.shader -> engine.core
engine.math -> engine.core
engine.image -> engine.core, engine.math (+ PRIVATE mango-image)
engine.mesh -> engine.core, engine.math (+ PRIVATE mango-import3d)
```

`engine.core` supplies types, allocators, containers, concurrency, services,
I/O, and HAL. `engine.math` wraps Mango math. `engine.shader` compiles Slang
shaders; `engine.rhi` wraps Slang-RHI; `engine.app` supplies application,
platform, input, window, player, scene, UI, and renderer integration. HAL
platforms are Windows, Linux, Darwin, and Generic.

`ApplicationDomain` explicitly declares whether an application is headless,
interactive, presence-aware, rendering, and UI-capable; it is immutable after
construction. Client/editor code owns
scene, player, camera, viewport, and UI state; it submits work to the renderer
rather than transferring that ownership to it.

Tests are split between GLFW-free `engine.tests.core` and GLFW-dependent
`engine.tests.app`; shared test support is `engine.tests`. Tests use the
test-only `"pP/UnitTest.h"` header and `PPR_UNIT_TEST`/`PPR_TEST_ASSERT`.

Read the root `codemap.md` before working; read a directory's `codemap.md` for
work in that area. Treat generated/dependency directories as excluded from
normal searches: `out/`, `_deps/`, `vcpkg_installed/`, `cmake-build-*/`,
`build/`, and `imgui_module_bindings/`.

## Architecture and module boundaries

- Dependencies may only point down the graph above. A lower layer must not
  import, include, resolve, or otherwise depend on a higher layer.
- Keep features in narrow module partitions with one coherent responsibility.
  Do not make umbrella modules or catch-all partitions carry feature logic.
- A `.cppm` exports declarations and necessary definitions for exported
  templates and inline functions. Non-inline, non-template definitions belong
  in the matching `.cpp` unless explicit instantiation closes the supported
  types. Follow the module
  declaration, partition, umbrella re-export, and CMake-registration procedure
  in **`module-architect`**. It is also the authority on `import std` policy.
- A new source file and its CMake registration are one change. CMake target
  names normally match dotted module names; use **`build-system`** for target,
  preset, dependency, sanitizer, or linker work.
- Never set `CMAKE_CXX_MODULE_STD` globally: it initializes fetched and
  subproject targets. Only PPR targets opt in through `setup_ppr_project()`;
  external exceptions set `CXX_MODULE_STD OFF` locally.
- Export only a deliberate, stable consumer contract. Keep implementation
  types, helpers, storage choices, and feature-local details unexported.
  Prefer a narrow exported interface over exporting a convenient dependency.
  Public API changes require tests and an explicit compatibility decision.

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
- Renderer boundary types are camera-free. Scene-owned `SceneView` pairs a
  `CameraSnapshot` with a `RenderView`; passes consume that snapshot and must
  not consult a mutable `Camera` while drawing.
- Geometry derives from `SceneView::m_render_view`. The camera/matrix contract
  is row-major, row-vector, left-handed, +Z-forward, +Y-up, with `[0,1]` depth:
  use `mul(float4, matrix)`, view-projection `view * projection`, and no
  backend-specific transpose or Y flip. See `App.Scene.Camera` and
  `Shader.cpp` for the canonical implementation.

## Tests

- Test externally observable behavior, contracts, error paths, lifetime and
  teardown effects—not private implementation structure.
- Keep core tests GLFW-free; put platform/window behavior in app tests. Add or
  update focused tests with a behavior/API change. Use `PPR_TEST_ASSERT`, which
  remains functional in release builds.
- Use **`unit-test-updater`** for test changes and **`validation`** for the
  post-change build, test, inspection, and diff checklist.

## C++ and API rules

- Prefer `constexpr`, `[[nodiscard]]` for meaningful results, and `noexcept`
  where truthful. Use engine integer aliases, sentinel values, strong
  `Numeric` wrappers, and `safe_narrowing` where they encode the contract.
- Prefer algorithms/ranges over raw loops. Use only macros from
  `include/pP/Macros.h`, except the test macros in `pP/UnitTest.h`.
- Functions access the outside world only through their signatures. Inject time,
  I/O, RNG, services, and mutable state at high-level boundaries.
- Accept the weakest useful input (views/spans and individual fields rather
  than owning containers or "wallet" aggregates). Use strong types or a named
  parameter struct when they prevent misuse.
- Encode important invariants and ordering requirements in types where that
  improves correctness. Keep every function body at one abstraction level;
  split work into named helpers rather than mixing orchestration and mechanics.
- Framework hooks (`main`, application hooks, listeners) are thin glue that
  delegates to engine logic.

## Source format

The active project CLion C/C++ Code Style is canonical for mechanical
formatting; invoke it manually through Reformat Code and through
`clion_reformat_file` as an agent. Every `clion_reformat_file` call MUST pass
`projectPath="E:/Code/ppr"` so the Project scheme resolves; the final diff
review rejects any whitespace-only hunks outside the functional edit. Agents must
not manually alter whitespace, line wrapping, indentation, blank lines, or
brace placement as part of a functional patch. Apply these additional rules:

- Use `const T` and `T *const`/`const T *const` when the callee must not reseat
  a pointer. Put `[[nodiscard]]`/other attributes, then inline-control macros,
  then `constexpr`, return type, name, parameters, `const`, and `noexcept`.
- Place `[[likely]]`/`[[unlikely]]` between a condition and its opening brace.
  Use `not` for new boolean negation; match surrounding `and`/`or` style.
- Order class members: `static_assert`s, data, static traits/constants, default
  constructor, copy/move, other constructors, destructor, accessors, mutators,
  comparisons. Keep private nested types immediately above the data they serve.
- Use comments only for invariants or non-obvious intent. Use a three-line
  divider only between genuinely separate conceptual regions.
- Use `#if PPR_ENABLE_*`, not `#ifdef`, for feature values. Keep top-level
  preprocessor directives at column zero.

## Specialist authorities and workflow

Use the named skill instead of reproducing its procedure here:

| Need | Authority |
|---|---|
| Code search, IDE build/run/debug/diagnostics | `clion-tools` |
| Modules, exports, partitions, `import std` | `module-architect` |
| CMake, presets, dependencies, sanitizers | `build-system` |
| Slang shaders, reflection, CPU/GPU layouts, and Slang-RHI bindings | `slang-shader-developer` |
| HAL changes | `hal-developer` |
| Allocators and `safe_ptr` mechanics | `memory-allocator` |
| Concurrency and async I/O | `concurrency-patterns` |
| Test updates | `unit-test-updater` |
| Validation | `validation` |
| Reviews, commits, pushes, deep work, worktrees | Their named OMO skills/commands |

Use CodeGraph first when the repository is indexed; otherwise use the
CLion-first fallback described by `clion-tools`. When dispatching work that
uses `clion_*` tools, spell out `projectPath="E:/Code/ppr"` in the dispatch
prompt; the MCP server does not infer it (only the three auto-detect tools
listed in `clion-tools` are exempt). Use PowerShell on Windows for
shell work. Temporary artifacts belong in `.slim/tmp/` (repo-scoped) or
`C:\Users\bek4b\AppData\Local\Temp\opencode\` (external). Do not commit,
push, or change generated/configuration files unless explicitly requested.

## Repository Map

A full codemap is available at `codemap.md` in the project root.

Before working on any task, read `codemap.md` to understand:
- Project architecture and entry points
- Directory responsibilities and design patterns
- Data flow and integration points between modules

For deep work on a specific folder, also read that folder's `codemap.md`.
