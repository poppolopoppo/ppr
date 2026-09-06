# AGENTS.md: Developer Guide for PPR Game Engine

Agent/skill conventions below follow oh-my-opencode-slim (OMO). If tool names or default grants stop matching your
installed version, check
`docs/skills.md` in your OMO install for drift.

## Agent Roster (OMO-provided, not custom)

These are the actual OMO agents — don't invent parallel names for them.

| Agent          | Role                                                              | Default skills | Default MCPs                         |
|----------------|-------------------------------------------------------------------|----------------|--------------------------------------|
| `orchestrator` | Plans, delegates, reconciles background specialists               | `["*"]`        | `["*", "!context7"]`                 |
| `explorer`     | Fast codebase recon                                               | `[]`           | `[]`                                 |
| `oracle`       | Architecture judgment, hard debugging, code review                | `["simplify"]` | `[]`                                 |
| `council`      | Multi-model consensus (`@council <question>`) — manual, high-cost | config-driven  | —                                    |
| `librarian`    | External knowledge (web, docs, dependency source)                 | `[]`           | `context7`, `gh_grep`, `codegraph`, `searxng`, `crawl4ai` |
| `designer`     | UI/UX — not generally relevant to PPR engine-core work            | `[]`           | `[]`                                 |
| `fixer`        | Bounded implementation                                            | `[]`           | `[]`                                 |

Per-agent skill and MCP allowlists are defined by
`.opencode/oh-my-opencode-slim.json`; an agent can only activate a granted skill or MCP. MCP registrations come from
the merged OpenCode configuration. Verify live availability with `opencode mcp list`. The active `librarian` MCP grants
include `context7`, `gh_grep`, `codegraph`, `searxng`, and `crawl4ai`. Generic `websearch` and Exa are not part of its
configured workflow. `oracle`'s default only includes `simplify`; the custom PPR skills below that route review work to
`oracle` (`code-reviewer`) need an explicit grant.

Caveat: the table above is simplified — `explorer` also carries `git-log-fast-navigation` plus CLion/codegraph
access, `oracle` carries 5 skills (`simplify`, `code-reviewer`, `concurrency-patterns`, `memory-allocator`,
`hal-developer`), and skill/MCP grants are per-preset blocks in `.opencode/oh-my-opencode-slim.json`, not one
global roster.

## Bundled OMO Skills — use these instead of reinventing them

| Skill                 | Purpose                                                                     | Invoke                                   |
|-----------------------|-----------------------------------------------------------------------------|------------------------------------------|
| `codemap`             | Hierarchical `codemap.md` repo maps, change-detected                        | `run codemap`                            |
| `clonedeps`           | Clone pinned dependency source into `.slim/clonedeps/repos/` for inspection | `clone dependencies`                     |
| `deepwork`            | Structured multi-phase workflow with mandatory Oracle review gates          | `/deepwork <task>`                       |
| `worktrees`           | Isolated `.slim/worktrees/<slug>/` lanes for risky/parallel work            | `work in a worktree`                     |
| `simplify`            | Behavior-preserving clarity refactor (owned by `oracle`)                    | ask for simplification, or during review |
| `reflect`             | Turns repeated friction into a reusable skill/agent/config change           | `/reflect`                               |
| `oh-my-opencode-slim` | Configure the plugin itself                                                 | ask to tune your setup                   |

Run `/reflect` periodically, especially after adding a new custom skill — it catches recurring workflow friction that's
really just a bundled skill (`codemap`, `clonedeps`, `@council`, etc.) waiting to be used instead of reinvented.

## Custom PPR-Specific Skills

Only these are genuinely PPR-domain-specific (no OMO bundled skill covers engine internals). Load on demand via the
`skill` tool; grant via
`skills: [...]` per agent as above. Each `SKILL.md` uses only `name` +
`description` frontmatter — that's the real OpenCode skill schema; don't add a `triggers:` field, it isn't read by
anything and just creates a second, driftable source of truth alongside `description`.

| Skill                     | Coverage                                                                                                                                                                                                                                                                           |
|---------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `clion-tools`             | CLion MCP tools for code search, debugging, building, diagnostics — use INSTEAD of grep/glob/bash                                                                                                                                                                                  |
| `memory-allocator`        | Allocator selection, composition, arena patterns, poison API, STL adapter, safe_ptr                                                                                                                                                                                                |
| `module-architect`        | Module naming, file structure, umbrella registration, CMake registration, pitfalls                                                                                                                                                                                                 |
| `build-system`            | Presets, setup_ppr_project, deps (CPM/vcpkg), MSVC workarounds, sanitizers                                                                                                                                                                                                         |
| `build-introspection`     | Read-only CMake File API reports for configured targets, C++ module file sets, toolchains, and dependency graphs                                                                                                                    |
| `hal-developer`           | 10 areas across 4 platforms, syscall mapping, stub conventions, adding a platform                                                                                                                                                                                                  |
| `concurrency-patterns`    | RawChannel MPSC, IEvent/Signal, IContext tree, thread safety, HAL I/O integration                                                                                                                                                                                                  |
| `code-reviewer`           | Review against AGENTS.md conventions, C++ best practices, safe_ptr lifetime — needs `oracle` skill grant (see above)                                                                                                                                                               |
| `git-commit-planner`      | Bounded commit planning, verified replay artifacts, explicit plan-ID confirmation gate, atomic ordering                                                                                                                                                                              |
| `git-push-planner`        | Squash planning, commit-message review, pre-push checklist                                                                                                                                                                                                                         |
| `git-log-fast-navigation` | Fast log formatting, rg-based commit/diff search, fzf browsing, .gitconfig aliases                                                                                                                                                                                                 |
| `unit-test-updater`       | Analyze diffs, update/add C++ unit tests                                                                                                                                                                                                                                           |
| `validation`              | Post-change checklist: compile every platform-relevant config, run engine tests, review diffs. Complements — doesn't replace — `deepwork`'s per-phase Oracle review gate; use `validation` standalone for a normal change, let `deepwork` invoke its own gate for multi-phase work |

Multi-phase refactoring and platform-porting work is handled by `deepwork`
(see [Risky / Multi-Phase Work](#risky--multi-phase-work)), not by a standalone plan-execution skill — a custom
top-level equivalent would just duplicate its plan → Oracle-review → phased-execution loop.

## Available Commands

| Command   | Coverage                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                |
|-----------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `/review` | Launches the `code-reviewer` skill against the current git diff. The skill handles its own subagent routing (diff retrieval via `@explorer`, 9-dimension reviews via background `oracle` subagents, per-finding validation, resolution gate via `@fixer`/`@oracle`). Distinct from `@council`: same model family, but `@council` runs N independent models in parallel for consensus, while `/review` runs N parallel reviewers on the same diff with correlated findings.                                                                                                                                                                                                                                              |
| `/commit` | Launches the `git-commit-planner` skill against the current working tree. It plans non-mutatingly, obtains grouping + dependency-ordering judgment via background `@oracle`, and emits a verified replay artifact (`.slim/commit-plan.json`) with a deterministic plan ID, exact payloads, and complete pre-execution snapshot. Execution is permitted only through `Invoke-CommitPlan.ps1` after the agent presents the verified plan ID, ordered commits, exact approved paths, messages, and freshness status and receives explicit confirmation for that plan ID. Scope or grouping approval never authorizes execution. |
| `/push`   | Launches the `git-push-planner` skill against the current branch's unpushed commits. The skill handles its own subagent routing (git state via CLion MCP / direct `bash`, secret-pattern scan via background `@explorer`, squash-pattern detection via background `@oracle`). Produces an agent-executable squash/rebase plan (`.slim/push-plan.json`) with a 4-action enum (`pick`/`squash`/`drop`/`reword`), revised messages, non-interactive executor commands, and a `pre_execution_snapshot` for drift detection; the skill **never** runs `git push`, `git rebase`, `git reset`, `git cherry-pick`, or `git commit --amend` — the user retains the final `git push --force-with-lease` op as the trust boundary. |
| `/plan`   | Launches the `verification-planning` skill for a proposed non-trivial change. The skill handles its own subagent routing (research into unfamiliar dependencies via `@librarian`, codebase invariant/inputs scan via `@explorer`, feasibility + cost review via `@oracle`). The skill **never** builds or edits to gather evidence itself — it plans the evidence path, then later work follows it.                                                                                                                                                                                                                                                                                                                     |

For architecture decisions or complex refactor proposals, use
`@council <question>` directly rather than a custom multi-proposal command — Council already runs N independent models
in parallel and synthesizes one answer, with real provider diversity a single-model command can't match.

## Architecture Overview

Module dependency chain (entry point). `engine.app` depends on `engine.core`/`engine.math`/`engine.rhi`;
`engine.rhi` imports `engine.shader`:

```
game/main.cpp → engine.app → engine.core  (foundation)
                            → engine.math  (vector math)
                            → engine.rhi   (GPU, imports engine.shader)
```

### engine.core — Foundation Library (`lib/engine/core/`)

31 module partitions providing all fundamental abstractions:

- **Types & Safety** (`Core.Types.cppm`): `u8`-`u64`/`i8`-`i64` shorthands, sentinel values (`default_value_v`,
  `zero_v`, `none_v`, `max_v`, `min_v`), `Numeric<T,TagT>` strong wrapper, `hash_t`, `relocatable<T>` trait
- **Containers**: `Stack<T,N>`, `RingBuffer<T,N>`, `SparseVector<T>`, `StableVector<T>`, `HashMap<K,V>`,
  `FlatSet<K>` (ordered), `FlatMap<K,V>` (`using FlatMap = std::flat_map`), `Bitmask<T,N>`, `ArrayView`,
  `RelativeView`, `TransformView`, `RelPtr`, `TagPtr`
- **Memory**: Allocator concepts (`TAllocator`, `TOwningAllocator`, `TResizableAllocator`, `TBlockAllocator`,
  `TArenaAllocator`, `TSlabAllocator`), `GPA` (operator new), `OS` (page alloc), `PMR` (polymorphic dispatch),
  `HugePage` (2 MiB pools), `SmallPage` (32/64 KiB pools), `extern template Arena` / `ScratchPad` (TLS),
  `PagePool`/`LocalCache`/`HintedPooling`, composite pieces (`InSituSlab`, `InSituFallback`/`InSituThreshold`
  aliases, `Fallback`, `Threshold`, `Pooling`, `HintedPooling`), `Allocation<T,A>`, `Allocator<A>`,
  `STL<A>` adapter, poison/ASAN annotations
- **Concurrency** (3 partitions): `RawChannel` (lock-free MPSC), `IEvent`/`ISignal`/`Signal<Events...>` (compile-time
  event multiplexing), `IContext`/`SharedContext` (Go-style cancellation tree)
- **HAL** (`Core.HAL.cppm`): Platform abstraction over `pP::hal` — `page*` memory, `ringBuffer*` buffer,
  async I/O, file watching, process spawning, debugger, deadline timers, native string transcoding. Implemented
  per-platform in
  `lib/engine/core/hal/<platform>/` (windows, linux, darwin, generic — Windows has 13 files, Linux/Darwin/Generic have
  10 each)
- **IO** (3 partitions): `IoFile`/`IoRequest`/`IoPort` wrappers over `hal::io` (submit/poll/wait async I/O,
  memory-mapped files, directory watching)
- **Services** (`Core.Service.cppm`): `IService` base with compile-time `typeUid<T>()` hash key, `ServicesStore`
  (thread-safe `FlatMap` with parent-chain fallback), `ServiceInjector` for implicit DI
- **Other**: `Logger`, `TimerManager`, `UnitTest` framework, `Callback<T>` with RAII Handle, `function_ref`, `Opaque`
  (variant/persistent/unique values and builder), string utilities, hashing infrastructure

### engine.math — Vector Math (`lib/engine/math/Math.cppm`)

Wraps `mango::math` into `namespace pP`:

- Type aliases: `float2/3/4`, `int2/3/4`, `uint2/3/4`, `float3x3`, `float4x4`
- Functions: `dot`, `dot2`, `lerp`, `normalize`, `distance`, `vector_cast`, `checked_cast` for vectors
- Matrix ops: `inverse` re-exported; other transforms come from callers (no `translate`/`scale`/`rotate`/`lookat`/
  `affineInverse`/`adjoint`/`oblique` in `Math.cppm`)
- Integration: `hashValue()` for hashing, `opaqueValue()` for serialization

### engine.rhi — GPU Abstraction (`lib/engine/rhi/RHI.cppm` + `RHI.cpp`)

Wraps Slang-RHI into `namespace pP::rhi`:

- Core types: `IDevice`, `IAdapter`, `IBuffer`, `ICommandBuffer`, `ICommandQueue`, `IRenderPipeline`,
  `IComputePipeline`, `IShaderProgram`, `ITexture`, `ITextureView`, `ISurface`, `IFence`, `IHeap`, `IInputLayout`
- Descriptors: `BufferDesc`, `DeviceDesc`, `RenderPipelineDesc`, `ShaderProgramDesc`, `SurfaceConfig`, etc.
- Projection helpers: `getOrthoMatrix()` and `getPerspectiveMatrix()` — common Mango-native left-handed, row-major,
  row-vector matrices with [0,1] depth shared by all renderers
- `IRhiService` interface — singleton service pattern wrapping `rhi::IRHI` and `rhi::IDevice` lifecycle;
  `createRenderPipeline()` virtual for pipeline creation from a render pass

### engine.shader — Shader Compilation (`lib/engine/shader/Shader.cppm` + `Shader.cpp`)

Provides Slang shader compilation:

- `IShaderService` interface — singleton service pattern wrapping `IGlobalSession`/`ISession` lifecycle
- `ModuleHandle` — RAII wrapper (`SharedModule`) for compiled shader modules
- File loading via `io::mapFile` (no file-watch hot-reload, no background compile thread)
- Imported by `engine.rhi` (for `IShaderProgram` creation); `game/main.cpp` imports core/math/rhi/app only

### engine.app — Application Layer (`lib/engine/app/`, 27 `.cppm`: umbrella + 26 partitions)

- **Application** (`App.Application.cppm`): Main loop class with virtual `initialize()`/`update()`/`render()`/
  `shutdown()`, service store, per-frame timing, directory resolution (install/config/content/working;
  `m_configDir` getter-only, no exit-code member), child `m_ui_services` store chained to the root store —
  services registered there are visible only to their viewport, with parent-chain fallback
- **Input** (5 partitions: action/device/filtered_analog/key/listener): `IInputService` — keyboard/mouse/gamepad
  device states, listener set (`addInputListener`/`removeInputListener`), action/mapping system (`InputMapping`
  binds keys to `InputAction` with `InputModifierEvent`/`InputTriggerEvent` callbacks), device enumeration
- **Window** (3 partitions: handle/monitor/viewport): `IWindowService` — monitor enumeration, window
  creation/destruction/resize/move, event callbacks (`whenWindowResized`, `whenWindowFocused`, etc.)
- **Player** (2 partitions): `IPlayerService` — player identity management, graph-based state machine (`Player::Graph`),
  keyboard/gamepad player binding
- **Viewport** (`window/App.Window.Viewport.cppm` → `:window.viewport`: `BasicRect`/`ViewportLayout`/`Viewport`/
  `WindowViewport`; `renderer/App.Renderer.Types.cppm`: `DrawSubmission`/`RenderView`): per-viewport render
  abstractions with per-entry scissor.
  Implementation note: `Viewport.cpp` uses `module engine.app; import :window.viewport;` (never
  `module engine.app:window.viewport;`)
- **Platform**: GLFW backend (`platform/glfw/`, 9 files) implementing `IPlatform`, `IInputService`, `IPlayerService`,
  `IWindowService`
- **Renderer** (`App.Renderer.cppm`): `Renderer` class — `initialize(IRhiService&)` sets up the pipeline;
  `renderAndPresent`/`submitToTexture` submit frames, per-entry scissor applied in `encodeDraws_` (`App.Renderer.cpp`)

### Key Design Patterns

- **Service Locator**: `IService` → compile-time `typeUid<T>()` → `ServicesStore` with parent-chain walk →
  `ServiceInjector` for implicit dependency injection. Safe via `safe_ptr<T>` (debug: ref-counted lifetime check,
  release: raw pointer).
- **Allocator Composition**: Concepts tiered from `TAllocator` up to `TSlabAllocator`. Concrete allocators composed via
  `InSituSlab` (inline storage), `Fallback<A,B>` (try A, then B), `Threshold<N,A,B>` (small→A, large→B), `Pooling<N,A>`
  (pool from A), `LocalCache<N,A,C>` (TLS cache over pool), `HintedPooling`. Wrap with `Allocator<A>` (type erasure),
  `PMR` (vtable dispatch), `STL<A>` (std:: adapter).
- **Event Multiplexing**: `IEvent` base → `Signal<Events...>` with compile-time composition and
  `std::counting_semaphore` → `select(events...)` Go-style helper for range-for over events.
- **Lock-free MPSC**: `RawChannel` for inter-thread message passing without mutex contention.
- **Cancellation Tree**: `IContext`/`SharedContext` — Go-style context propagation with deadline support.
- **Opaque Serialization**: `opaque::Value` (type-erased variant), `opaque::Block` (persistent byte buffer),
  `opaque::Unique` (RAII owning handle), `Block::Builder` (serialization builder).

### Test Infrastructure

Two separate test executables:

- `engine.tests.core` (`lib/engine/tests/core/`) — GLFW-free; tests memory, containers, concurrency, IO, strings, utility,
  opaque, enums (`Core.Service.Tests.cppm` exists as a `:service` module but is not wired into `Core.Tests.cppm`
  — no `:service` import/recurse)
- `engine.tests.app` (`lib/engine/tests/app/`) — links GLFW for platform-dependent tests

Shared in `lib/engine/tests/shared/` as static lib `engine.tests` providing `parseCli()` and `runSuite()` to avoid
duplication. Tests use `PPR_UNIT_TEST(name)` macros compiled as `inline constexpr` variables with `UnitTest` tree
grouping, fork/crash support, and `--run-test --shuffle --loop` CLI. Test code includes the test-only header
`"pP/UnitTest.h"` (from `lib/engine/tests/include/`, registered per test target) which provides `PPR_UNIT_TEST`,
`PPR_TEST_ASSERT` (functional in release builds — always throws, unlike engine `PPR_ASSERT` which compiles to
`[[assume]]`), and `PPR_UNIT_TEST_ERRC` (error-code opt-in bodies).

### Entry Point (`game/main.cpp`)

Imports core/math/rhi/app engine modules (engine.shader arrives via engine.rhi), constructs `pP::Application(name, argv)`, calls `app.run()`. The application resolves
install/config/content/working directories, discovers and initializes registered services (input, window, player, RHI,
shader), then runs the per-frame update/render loop until exit.

## External File Loading

When you encounter a file reference (e.g., @rules/general.md), load it on demand. Do NOT preemptively load all
references. Treat loaded content as mandatory instructions.

## Web Research

- Use `context7` only when it resolves the exact library and version. Use `searxng` only to discover authoritative URLs;
  stop once the official documentation or repository is identified. Do not use generic `websearch` or Exa.
- Use `gh_grep` first for GitHub source discovery. Once the repository, revision, and path are known, use `WebFetch`
  only for raw GitHub content.
- Use `crawl4ai_md` for known HTML pages and `crawl4ai_crawl` for multiple HTML pages. Use `WebFetch` only for raw text,
  APIs, binaries, or after Crawl4AI fails or is unsuitable. Stop after obtaining adequate authoritative sources, and cite
  only sources actually retrieved.
- When delegating research, specify this retrieval ladder and a discovery budget of one or two `searxng` calls unless
  additional discovery is demonstrably necessary.

## Tool Usage

Use tools in this priority order:

1. **CLion MCP tools** (`clion-*`) for code search, navigation, build, run, and debugging.
2. **Internal tools** (read/edit/grep/glob/task) for file and content operations — reading and editing known files,
   quick text search, subagent delegation.
3. **PowerShell (pwsh) only** for shell commands on Windows — never mix in other shells (Bash, cmd, Git Bash, etc.); use
   bash on Unix. `rg` (ripgrep) is an allowed exception for fast content search.

## Temporary Files

Single canonical temp for all agents — never improvise:

* **External (default):** `C:\Users\bek4b\AppData\Local\Temp\opencode\` — for downloads, `> file` pipes, and external tool outputs (`dot -Tsvg`, `curl -o`, etc.). Allowed via `permission.external_directory` — no prompt. Pre-created.
* **In-workspace (when repo-scoped):** `.slim/tmp/` — gitignored via `.slim/`, needs no permission. Create on demand: `New-Item -ItemType Directory -Path .slim/tmp -Force`.

Rules:
1. Never use `C:\Temp`, bare `%TEMP%` without `\opencode`, WSL `/tmp`, or repo root/docs for temp artifacts.
2. Use `C:\Users\bek4b\AppData\Local\Temp\opencode\<name>.ext` or `.slim/tmp/<name>.ext` consistently.
3. Prefer `.slim/tmp` for repo-scoped ephemeral, `...\opencode\` for cross-session cache. Clean large files after verification.

## Recon & Context (delegate to bundled skills first)

- **Repo map**: use `codemap` (`run codemap`) for hierarchical, change-detected
  `codemap.md` files instead of ad hoc project maps. It already handles incremental re-analysis of only changed
  folders — don't build a parallel cache for this.
- **Dependency internals** (Slang-RHI, mango::math, etc.): use `clonedeps`
  (`clone dependencies`). `orchestrator` asks `@librarian` to resolve the official repo/tag, confirms with you, then
  clones a pinned ref into
  `.slim/clonedeps/repos/` (max 3-5 deps, HTTPS + pinned refs only, no scripts run, kept out of git) and records it
  below.
- **Search scoping (default-exclude)**: mirror `.gitignore`. Exclude from all searches: `out/`, `_deps/`,
  `vcpkg_installed/`, `cmake-build-*/`, `build/`,
  `imgui_module_bindings`.
    - CLion MCP: pass `paths` excluding those dirs.
    - `rg` fallback: `rg --glob '!out/**' --glob '!**/vcpkg_installed/**' --glob '!**/_deps/**'`.
    - Once a dependency is cloned via `clonedeps`, read it from
      `.slim/clonedeps/repos/<name>/` directly rather than scoping into
      `vcpkg_installed`.

## Cloned Dependency Source

_(maintained by the `clonedeps` skill — empty until first run)_

## Risky / Multi-Phase Work

- **Multi-phase refactors, new HAL platforms, new module libraries**: start with `/deepwork <task>`. It creates a
  session artifact in
  `.slim/deepwork/<task>.md`, gets an Oracle review on the draft plan, splits it into phases (each with its own Oracle
  review), and executes phase by phase with validation between phases. PPR-specific mechanics — the module partition
  checklist, the CMake registration rule, the platform-file checklist below — are what `@fixer` follows *inside* each
  `deepwork` phase; they are not a competing top-level workflow.
- **Isolated lanes for risky or parallel work**: use `worktrees`
  (`work in a worktree`). Sets up `.slim/worktrees/<slug>/`, tracked in
  `.slim/worktrees.json`, with pre-flight dirty-tree checks and confirmation gates on every git mutation
  (`worktree add/remove`, `merge`, `rebase`,
  `cherry-pick`, `reset --hard`, branch ops). Do the actual implementation there, then hand off to `git-commit-planner`/
  `git-push-planner` for the atomic-commit and pre-push pass before `worktrees` integrates back.
- Keep `/review` (4-dimension parallel review) and Oracle's `deepwork`/`validation`
  gates for high-risk changes (memory, concurrency, build-system, new HAL platform).

### Session reuse (keyed + invalidated)

- Reuse a specialist session only when its **session key** matches: `(agent-type, target area, file-glob)`. MRU is a
  tiebreaker only.
- **Invalidate** sessions older than a threshold or whose key no longer matches the current task.
- **Never reuse mutating/debug sessions** (breakpoints, watches, state) — prefer fresh for debug; read-only recon
  sessions are safe to reuse.

### Targeted reads

- `read` with `offset`/`limit`; never dump whole large files.

### Tool priority (fallback ladder, not exclusive)

1. CLion MCP tools (`clion_search_symbol`, `clion_search_text`, `clion_get_compiler_info`) — language-aware, respects
   includes.
2. Internal `grep`/`glob` with exclusions.
3. `pwsh` + `rg` with exclusion globs (last resort).

- First `clion_search_*` after launch may be unindexed — tolerate.
- Canonical tool catalog lives in `clion-tools` SKILL.md; reference it, do not duplicate tool names here (avoid drift).

## Build System

- Load the `clion-tools` skill when starting any task. Follow the Tool Usage priority: CLion MCP tools for code search,
  building, and debugging; internal tools for file and content operations.
- Use `build-introspection` for generated target, module, dependency, or compiler facts; read the existing
  `out/build/msvc-dev` CMake File API replies before configuring or reconstructing metadata from CMake sources.
- CMake 4.3+, C++23, modules enabled, experimental `import std`.
- Presets: `msvc-dev` (recommended), `msvc-live` (Debug Edit&Continue, no ASAN), `msvc-rel`, `clang-cl-dev`,
  `clang-cl-rel`, `clang-dev`, `clang-rel`, `gcc-dev`/`gcc-rel` (hidden, no modules). These 9 are the curated
  set — extra presets exist (`default`, `developer`, `vcpkg`, `windows`/`unix-like-default`).
- Use `setup_ppr_project(Target INTERNAL_PUBLIC_DEPS ... EXTERNAL_SYSTEM_PRIVATE_DEPS ...)` for every target (see
  cmake/Compilers.cmake).
- Commit rule: new source file + its CMakeLists.txt registration go in the same commit.
- CMake target == C++ module name (dotted): `engine.core`, `engine.math`, `engine.shader`, `engine.rhi`, `engine.app`,
  `engine.tests`, `engine.tests.core`, `engine.tests.app`. Exceptions: `app.game` (module-less game executable),
  `run-engine-tests` (hyphenated aggregate, kept), external imported targets (`rapidhash`/`stb`/`mango`/`glfw`/
  `slang-rhi`/`slang`),
  `imgui` module binding over `imgui.base` static (root-scope `CXX_MODULE_STD OFF` workaround preserved in
  `cmake/external/DearImGui.cmake`).
- Two separate test executables: `engine.tests.core` (core, GLFW-free) and `engine.tests.app` (links glfw). Aggregate target
  `run-engine-tests` runs both. Build via `cmake --build out/build/msvc-dev --target engine.tests.core` (or
  `engine.tests.app`). Run via `run-engine-tests` run configuration in CLion.
- Shared test infrastructure in `lib/engine/tests/shared/` (static lib `engine.tests`) provides `parseCli()` and
  `runSuite()` to avoid duplication between test executables.

## C++20 Modules

- `.cppm` = interface (exports), `.cpp` = implementation (definitions), `.h` = Macros.h only.
- Libraries: `engine.core`, `engine.math`, `engine.shader`, `engine.rhi`, `engine.app`.
- Partitions: `engine.core:containers.hash_map` (dots = hierarchy).
- Tests: `engine.tests.core:memory` (dots for module name, colon for partition).
- File naming: `Core.<Partition>.cppm` / `Core.<Partition>.cpp`, platform HAL: `Core.HAL.<platform>.<Area>.cpp`.
- Interface pattern:
  ```cpp
  module;
  #include "pP/Macros.h"
  export module engine.core:your_partition;
  import std;
  export namespace pP { ... }
  ```
- Implementation pattern:
  ```cpp
  module;
  #include "pP/Macros.h"
  module engine.core;
  import :your_partition;
  import std;
  namespace pP { ... }
  ```
- Umbrella re-exports: add `export import :partition;` to `Core.cppm`.
- Keep `.cppm` files minimal (exports only); put definitions in `.cpp`.

## Coding Standards

- No raw loops (prefer algorithms/ranges). Comments should be exceptional — only add them for genuinely surprising or
  non-obvious code that cannot be clarified through naming or structure alone.
- `constexpr` everywhere, `[[nodiscard]]` on important returns, `noexcept` where possible.
- Inlining: `PPR_FORCE_INLINE` (hot paths), `PPR_NO_INLINE` (prevent), `PPR_FLATTEN` (recursive).
- Attributes: `PPR_EMPTY_BASES` (MSVC stateless wrappers), `PPR_LIFETIME_BOUND` (reference lifetime deps).
- Allowed macros: only those in `include/pP/Macros.h` — assertions (PPR_ASSERT/VERIFY/ENSURE/ASSUME), PPR_DEFER,
  inlining control, logging (PPR_LOG), and internal helper macros (stringize, concat, pragma, etc.). Test-only macros
  (`PPR_UNIT_TEST`, `PPR_TEST_ASSERT`, `PPR_UNIT_TEST_ERRC`) live in `lib/engine/tests/include/pP/UnitTest.h`, not in
  `Macros.h`. No macros from other sources.
- Function & API design follows §Function Design Principles below — honest signatures, empathetic parameters,
  type-encoded contracts, one abstraction level per body.

## Source Formatting

C++ files are auto-formatted by clang-format (`.clang-format` at the repo root, LLVM base style, `IndentWidth: 4`,
`ColumnLimit: 120`,
`BreakBeforeBraces: Custom` with all `BraceWrapping.*: false`,
`AlignOperands: true`, `NamespaceIndentation: All`). Don't hand-format what the formatter already covers. This section
documents only what is **specific to this repository and not enforced by the formatter**.

### Section dividers

Three-line `// ----` block (a `//` line with a long dash run close to
`ColumnLimit`) separates conceptual regions inside a file. Topic in lowercase on the middle line. Indent matches the
enclosing scope. The first divider after a namespace opening has no blank line above; every subsequent divider is
flanked by exactly one blank line.

```cpp
    // ------------------------------------------------------------------
    // allocator concepts
    // ------------------------------------------------------------------
```

### Function declaration prefix order

clang-format does not order C++ attributes or specifiers. The repo order:
`[[nodiscard]]` / `[[maybe_unused]]` / `PPR_FORCE_INLINE` → `constexpr`
(when applicable) → return type → name → params → `const` (member fns)
→ `noexcept`. Trailing-return variants keep the same qualifier positions.

```cpp
[[nodiscard]] PPR_FORCE_INLINE constexpr bool isValid() const noexcept;
```

### Pointer / const parameter style

The formatter enforces `type *name` spacing (LLVM default
`PointerAlignment: Right`). The repo layers two manual decisions on top:

- Place read-only qualifiers **before** the type: `const T`.
- Place "callee does not reseat" qualifiers **after** the `*`:
  `T *const name` for writeable fixed binding, or
  `const T *const name` for read-only fixed binding.

```cpp
[[nodiscard]] constexpr bool overlap(const void *const storage,
                                     const std::size_t bytes,
                                     const void *const ptr) noexcept;
```

### Branch attributes

`[[likely]]` / `[[unlikely]]` sits between the condition's closing paren and the opening brace, on the same line. Mark
fast paths / early exits
`[[likely]]`; rare OOM / unreachable / error early-exit `[[unlikely]]`.

```cpp
if (m_status == status_used_) [[unlikely]] {
    return {};
}
```

### Member declaration order in classes

clang-format does not order class members. The repo sequence (skip sections the type does not have):

1. `static_assert` block
2. Data members
3. Static constants / traits (`is_stateless_v`)
4. Default constructor
5. Copy/move special members (often `= delete` or with `requires` clauses)
6. Other constructors
7. Destructor
8. Trivial accessors (`isValid`, `data`, `count`, ...)
9. Mutators (`allocate`, `resize`, `deallocate`, `create`, `destroy`, ...)
10. Comparison operators at the end (`= default`)

Private members and nested types (`enum EStatus_ { ... }`) sit in the private section directly above the data they
describe.

### Comments

clang-format does not police comment content. Conventions:

- `///` for invariant-level docstrings on a declaration (e.g. `discard()`).
- `//` short intent lines above declarations when the signature is not self-explanatory.
- `//` inside function bodies is fine for algorithm steps, concurrency invariants, and hardware coupling in complex
  methods (multiple notes in `Core.Memory.Allocator.cppm:1019-1100`; hardware-backend note in
  `App.Input.Gamepad.cpp:25`).

### Boolean negation

clang-format does not enforce. Use `not` instead of `!` for boolean negation (the engine uses `not` exclusively in
boolean contexts). The compound `and`/`or` keywords are acceptable; existing `&&`/`||` in core/memory code is fine —
match the file you are editing.

### Conditional compilation

Use `#if PPR_ENABLE_*` over `#ifdef` so values, not just presence, are tested. Top-level `#if` at column 0. Nested `#if`
may be indented 4 spaces (the one outlier at `Core.Memory.cpp:90` is leaving a previously indented block — prefer column
0 in new code). `#endif // PPR_ENABLE_*`
is optional; add only when the block is long enough to benefit from a close-tag label.

### Module file split (.cppm vs .cpp)

clang-format does not enforce module architecture. The repo split:

- `.cppm` — declarations only; bodies go in the matching `.cpp`.
- `.cpp` — uses `module <lib>;` (the **umbrella**, never
  `module <lib>:partition;`) then `import :partition;`, optional
  `import std;`, then `namespace pP {`.
- `constexpr` functions stay **inline in the `.cppm`** — no `.cpp`
  definition is generated for them.
- `noexcept` **IS** repeated verbatim on `.cpp` definitions matching the
  `.cppm` declaration (e.g. `GPA::allocateRaw` at `Core.Memory.cpp:16`).
- `import std;` is added only when std types appear in the file.

## Function Design Principles

Normative rules for designing functions and APIs. Enforced by `code-reviewer`
Dimension 9; refactors and new code follow them too.

### Honesty

- A function accesses the outside world **only through its signature** — never reads or writes state invisible to
  callers (clock, global/static mutable state, conjured singletons).
- Dishonesty is infectious: a caller that invokes a dishonest function is dishonest itself, so honest functions sit at
  call-tree leaves.
- Maximize honest functions; **inject dishonesty at the topmost level**. I/O, time, RNG seeding, and service resolution
  happen near `Application` and service-store layers; core logic receives dependencies as parameters (pass a PRNG in and
  seed it dishonestly at the call site — never conjure a global generator inside).

### Empathetic signatures

- Accept the weakest thing you need: `span<T>`/views over concrete containers; individual fields over whole aggregates
  (no wallet anti-pattern).
- Use a marshalling parameter struct when many same-typed arguments would otherwise invite transposition errors.
- Prevent accidental conversions with strong types (`Numeric<T, TagT>`).

### Type-encoded contracts

- Encode preconditions as wrapper types establishing invariants at construction (normalized-vector style); make
  conversion back cheap/implicit.
- Encode ordering contracts as receipt parameters (e.g., pass the lock guard to prove the mutex is held).
- Specialize idempotent operations on invariant types; do not encode every pre/postcondition in types — know where the
  line is.

### Golden rule: one abstraction level per body

- Every line of a function body sits at the same level of abstraction; zooming in means calling another function.
- Section-labeling comments are the smell → split into brick functions. This is the rationale behind the no-raw-loops
  rule above.
- Encapsulate ad-hoc data structures maintained by hand across sibling functions (e.g., a case-insensitive name→asset
  map) instead of mirroring their invariants manually in each function.

### Thin framework hooks

- `Application::initialize/update/render`, `main`, input/window listeners delegate immediately into engine code; hooks
  glue, they don't work.

### Sketch-first workflow

- Call the function you wish existed at the right abstraction level, then go implement it ("write the functions you want
  to see in the world").

## Type Safety

- `safe_narrowing<IntT>` — tag type asserting round-trip on implicit conversion.
- Integer shorthands: `u8/u16/u32/u64/i8/i16/i32/i64` (Core.Types.cppm).
- Sentinel values: `default_value_v`, `zero_v`, `none_v`, `max_v`, `min_v`.
- `Numeric<T, TagT>` — strongly-typed numeric wrapper.
- `hash_t` — type-safe hash value (struct with m_value, comparison, hashValue).
- `pP::details::relocatable<T>` — mark types supporting memcpy.

## Assertions

| Macro              | Debug Behavior                             | Release Behavior              |
|--------------------|--------------------------------------------|-------------------------------|
| `PPR_ASSERT(expr)` | Calls onFailure, throws `std::logic_error` | `PPR_ASSUME(expr)`            |
| `PPR_VERIFY(expr)` | Calls onFailure, throws `std::logic_error` | Evaluates expr + `PPR_ASSUME` |
| `PPR_ENSURE(expr)` | Returns false on failure                   | `PPR_ASSUME(expr)` then eval  |
| `PPR_ASSUME(expr)` | `[[assume(expr)]]` / `__built_assume`      | Same                          |

- `PPR_ENABLE_ASSERTIONS` = `PPR_ENABLE_DEBUG` (`_DEBUG` or `!NDEBUG`).
- `Assertion::setFailurePolicy()` lets tests intercept assertions.

## HAL

Platform code in `lib/engine/core/hal/<platform>/`. Supported: windows, linux, darwin, generic (stub). Selected via
`PPR_HAL_PLATFORM` (cmake/HAL.cmake).

- `pP::hal`: pageAlloc/Free/Commit/Decommit/Protect/OfferToOS/ReclaimFromOS, ringBufferAlloc/Free, outputDebug,
  isDebuggerPresent, breakpoint.
- Sub-namespaces: `process` (executablePath, spawnAndWait), `timer` (setDeadline, cancelDeadline), `io`
  (async I/O, file watches), `native` (string transcoding).
- See `Core.HAL.cppm` for full API surface.

## Memory & Allocators

- Concepts: `TAllocator`, `TOwningAllocator`, `TResizableAllocator`, `TBlockAllocator`, `TArenaAllocator`,
  `TSlabAllocator` (in Core.Memory.Allocator.cppm).
- Hierarchy: GPA (operator new) → OS (pageAlloc) → PagePool/BitmapTree → HugePage (2 MiB) / SmallPage (32/64 KiB) →
  `extern template Arena` / ScratchPad (TLS transient).
- Slab/Arena: InSituSlab, `InSituFallback`/`InSituThreshold` aliases, ScratchPad (TLS Arena<SmallPage>).
- Composite: Fallback, Threshold, Pooling, LocalCache, HintedPooling, Allocation<T,A>, Allocator<A>, STL<A>.
- Poison: `poisonReserved`, `unpoisonUninitialized`, `poisonDestroyed`, `annotateContiguousContainer` (+ typed
  overloads). Uses `__asan_*` when ASAN enabled, debug patterns (0xAA/0xCC/0xDD) otherwise, no-op in release.
- For MSVC with `msvc-dev` preset, ASAN is auto-enabled via PPR_ENABLE_DEVELOPER_MODE.
- See `Core.Memory.*.cppm` for full type catalog.

## Core Abstractions

All types in `namespace pP`. See corresponding `.cppm` files:

- **Containers:** Stack<T,N>, RingBuffer<T,N> (bounded, trivial T); SparseVector<T>, StableVector<T>, HashMap<K,V>,
  FlatSet<K> (ordered), FlatMap<K,V> (`using FlatMap = std::flat_map`), Bitmask<T,N>, SetBitsRange.
- **Pointers/views:** RelPtr (relative offset), TagPtr (flagged), ArrayView, RelativeView (half-size), safe_ptr,
  IndexIterator.
- **Strings:** string_literal, static_string<N>, char helpers (toLower, etc.), lazy transforms (caseFold, stringEscape,
  trim, etc.).
- **Opaque values:** opaque::Value (variant), opaque::Block (persistent), opaque::Unique (RAII owning), Block::Builder.
- **Concurrency:** IEvent/ISignal/Signal<...>, RawChannel (lock-free MPSC), IContext/SharedContext (Go-style
  cancellation).
- **Other:** IService/typeUid<T>, Log::Category/ELevel/Emitter, TimerManager, overloaded (visitor), std23::function_ref.

### safe_ptr<T>

- **Debug mode** (`PPR_ENABLE_DEBUG`): reference-counted lifetime checker — asserts that no `safe_ptr` outlives the
  pointed-to object
- **Release mode**: zero-overhead raw pointer (identical to `T*`)
- **NOT** a shared ownership pointer — the user guarantees init/destroy ordering
- All `safe_ptr` copies must be released (set to `nullptr` or go out of scope) before the owning object is destroyed
- `safe_ptr` from `unique_ptr::get()` is correct by design: the user guarantees the `unique_ptr` outlives all `safe_ptr`
  instances; `safe_ptr` will assert if violated

## Unit Testing

- Define: `PPR_UNIT_TEST(name) { PPR_TEST_ASSERT(cond); };` (tests are `inline constexpr` variables).
- `PPR_TEST_ASSERT` is functional in all build configs (throws `std::logic_error` via `onTestAssertionFailure`) — tests
  must never use `PPR_ASSERT`/`PPR_VERIFY`, which compile to `[[assume]]` in release. Test files include
  `"pP/UnitTest.h"` (test-only header; not part of the engine).
- Flags: `UnitTest::expect_fail` (must throw), `UnitTest::fork` (child process), `UnitTest::expect_crash` (fork +
  expect_fail).
- Group: `_.recurse({TestA, TestB, ...})` — supports conditional inclusion via `if constexpr (PPR_ENABLE_DEBUG)`.
- Module pattern: `export module engine.tests.core:memory;` with `export namespace pP::tests { ... }`.
- CLI: `engine.tests.core` / `engine.tests.app` `[--run-test <path>] [--shuffle [<seed>]] [--no-shuffle] [--loop <N>] [--child-run] [--help]` — shuffle
  on by default; test paths and `Id`s use `/` separators (e.g. `--run-test core/hal/thread_id`), matched via
  `filterMatches`.
- Fork tests re-spawn via `hal::process::spawnAndWait` with `--child-run --run-test <path>`. Assertions intercepted by test framework (converted
  to failures, not terminations).
- Run programmatically: static `pP::UnitTest::run(Context, UnitTest)` with roots `pP::tests::core` / `pP::tests::app`.
- Optional error-code bodies: the ec-reporting `run()` overload takes `test_func_ec_t` functions returning an error
  code (no `PPR_TEST_ASSERT_ERRC` macro exists); `UnitTest::Context::m_fail_with` is the generic `Context` failure
  handler for message output.
- See `lib/engine/tests/` for existing examples.

## Debugging with CLion

- Prefer the debugger over printf/logging whenever the debugger is available.
- MSVC `/WX` build is ground truth; CLion inspections are advisory only.
- Goal is portable C++23; known module/BMI gaps in CLion analysis are acknowledged, not fixed by code churn.
- Research-first: confirm a diagnostic before editing; never fix an advisory-only finding blindly.
- Suppressions must be narrow with recorded justification; broad or unexplained silencing is not accepted.
- Load the `clion-tools` skill for the diagnostics procedure (focus, triage, confirm, suppress).

## Matrix Layout Conventions

**PPR conventions (verified):**

- **Storage**: Row-major (HLSL/DirectX compatible), set explicitly in `lib/engine/shader/Shader.cpp` via
  `session_desc.defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_ROW_MAJOR;`
- **Vector–matrix multiply**: `mul(float4, matrix)` — row-vector convention (matrix on the right), matching
  `mango::math`. Do **not** use `mul(matrix, float4)`, which transposes the transform, flips view-space Z, and yields
  negative W (clipped geometry).
- **Camera / shader**: `m_view * m_projection` for viewProjection; shader does
  `mul(float4(input.position, 1.0), g_frame.m_view_projection)`.
- **Coordinate system**: Mango-native left-handed view space with +Z forward and +Y up. NDC X/Y are [-1,+1] with +Y up;
  depth is [0,1] with near=0 and far=1. Client framebuffer coordinates are top-left with +Y down.
- **Projection**: Use Mango `lookat(target, viewer, up)`, `perspectiveD3D`, and `orthoD3D` for every backend. D3D12,
  Vulkan, Metal, and WGPU consume the same untransposed matrices; never add a backend-specific projection or Vulkan Y
  flip.

**Why row-major:** It is the only layout reliably portable across D3D, Vulkan, OpenGL, Metal, and CUDA. Slang's library
API defaults to row-major (the `slangc` CLI defaults to column-major); PPR overrides it at the session level. Per-target
layouts can be mixed via separate `SessionDesc`s.

**Handedness:** Slang-RHI does not enforce or override coordinate handedness (D3D traditionally left-handed,
OpenGL/Vulkan right-handed, Metal varies) — that is the math library's responsibility. Configure the host math library
to row-major and be explicit about vector interpretation for portability.

**Non-4x4 caveat:** Non-4x4 matrices (e.g. 4×3) may have size/alignment mismatches; insert a manual transpose at the
host-to-shader boundary if sizes differ.

**References**

- Slang user guide: https://docs.shader-slang.org/en/stable/external/slang/docs/user-guide/a1-01-matrix-layout.html
- `lib/engine/shader/Shader.cpp` (rows 158-164, 192-198)

## CMake Version Tracking

- **CMake 4.4 synthetic target genex leak**: Single-config Ninja (CMAKE_BUILD_TYPE per preset) is used to avoid CMake
  4.4's multi-config genex evaluation gap for C++ module synthetic targets. When CMake 4.5+ is adopted, test whether the
  `default` preset can switch back to `"Ninja Multi-Config"` without producing conflicting flags (e.g., `/Od` + `/Ox` in
  Debug synth targets). The `default` preset's description in CMakePresets.json contains a searchable reminder.
- **Root-scope module targets break `@cmake_cxx_std.lib` links**: CMake creates the synthetic `std` module target
  (`@cmake_cxx_std.lib`) in the directory scope of the first target that needs it (any target with
  `FILE_SET CXX_MODULES` when `CXX_MODULE_STD` is ON, regardless of whether its sources `import std;`). If that target
  lives in the TOP-LEVEL scope (e.g., a target defined via `include()`d cmake file rather than `add_subdirectory()`),
  the std library is referenced in link lines as the bare name `@cmake_cxx_std.lib` — the leading `@` is MSVC
  response-file syntax, so the linker drops it and every link fails with
  `LNK2001: unresolved external symbol std::_General_precision_tables_2<...>::_Max_P` (or similar std-module
  implicit-inline definitions). Fix: set `CXX_MODULE_STD OFF` on such targets (see `cmake/external/DearImGui.cmake`,
  where `imgui` triggered this via LNK2001 on `_Max_P`). When adopting a newer CMake, test whether root-scope
  module targets still produce a bare `@`-prefixed std lib reference.

## Repository Map

A full codemap is available at `codemap.md` in the project root.

Before working on any task, read `codemap.md` to understand:

- Project architecture and entry points
- Directory responsibilities and design patterns
- Data flow and integration points between modules

For deep work on a specific folder, also read that folder's `codemap.md`.

