# AGENTS.md: Developer Guide for PPR Game Engine

Agent/skill conventions below follow oh-my-opencode-slim (OMO). If tool
names or default grants stop matching your installed version, check
`docs/skills.md` in your OMO install for drift.

## Agent Roster (OMO-provided, not custom)

These are the actual OMO agents — don't invent parallel names for them.

| Agent | Role | Default skills | Default MCPs |
|---|---|---|---|
| `orchestrator` | Plans, delegates, reconciles background specialists | `["*"]` | `["*", "!context7"]` |
| `explorer` | Fast codebase recon | `[]` | `[]` |
| `oracle` | Architecture judgment, hard debugging, code review | `["simplify"]` | `[]` |
| `council` | Multi-model consensus (`@council <question>`) — manual, high-cost | config-driven | — |
| `librarian` | External knowledge (web, docs, dependency source) | `[]` | `["websearch","context7","gh_grep"]` |
| `designer` | UI/UX — not generally relevant to PPR engine-core work | `[]` | `[]` |
| `fixer` | Bounded implementation | `[]` | `[]` |

Skill assignment is a **permission grant** in `~/.config/opencode/oh-my-opencode-slim.json`
(or a project-local override) — an agent can only activate a skill it's been
given. `oracle`'s default only includes `simplify`; the custom PPR skills below
that route review work to `oracle` (`code-reviewer`) need an explicit grant:

```jsonc
{
  "presets": {
    "ppr": {
      "orchestrator": { "skills": ["*"] },
      "oracle": { "skills": ["simplify", "code-reviewer"] },
      "explorer": { "skills": [] },
      "fixer": { "skills": [] },
      "librarian": { "skills": [], "mcps": ["websearch", "context7", "gh_grep"] }
    }
  }
}
```

## Preset Hierarchy

The active preset is set by the top-level `"preset"` field in
`~/.config/opencode/oh-my-opencode-slim.json`. Switch at runtime with `/preset <name>`.

| Preset | Source | Cost | When to use |
|--------|--------|------|-------------|
| `ppr-free` | PPR custom (default) | $0 | Normal work — all-free models |
| `ppr-pro` | PPR custom (opt-in) | $$ | Architecture decisions, multi-phase refactors, high-stakes `@council` |
| `opencode-zen-free` | OMO curated | $0 | Fallback if `ppr-free` has issues |
| `opencode-go` | OMO curated | $$ | Fallback if `ppr-pro` has issues |
| `openai` | OMO curated | $$$ | Fallback if all else fails |

**PPR-specific deviations from OMO defaults:**
- `oracle` adds `code-reviewer` skill (PPR has extensive code review needs)
- `fixer` adds 6 PPR skills: `module-architect`, `memory-allocator`, `build-system`, `clion-tools`, `unit-test-updater`, `validation`
- `ppr-pro` council synth uses `minimax-m3` (different from oracle for diversity)

## Bundled OMO Skills — use these instead of reinventing them

| Skill | Purpose | Invoke |
|---|---|---|
| `codemap` | Hierarchical `codemap.md` repo maps, change-detected | `run codemap` |
| `clonedeps` | Clone pinned dependency source into `.slim/clonedeps/repos/` for inspection | `clone dependencies` |
| `deepwork` | Structured multi-phase workflow with mandatory Oracle review gates | `/deepwork <task>` |
| `worktrees` | Isolated `.slim/worktrees/<slug>/` lanes for risky/parallel work | `work in a worktree` |
| `simplify` | Behavior-preserving clarity refactor (owned by `oracle`) | ask for simplification, or during review |
| `reflect` | Turns repeated friction into a reusable skill/agent/config change | `/reflect` |
| `oh-my-opencode-slim` | Configure the plugin itself | ask to tune your setup |

Run `/reflect` periodically, especially after adding a new custom skill — it
catches recurring workflow friction that's really just a bundled skill
(`codemap`, `clonedeps`, `@council`, etc.) waiting to be used instead of
reinvented.

## Custom PPR-Specific Skills

Only these are genuinely PPR-domain-specific (no OMO bundled skill covers
engine internals). Load on demand via the `skill` tool; grant via
`skills: [...]` per agent as above. Each `SKILL.md` uses only `name` +
`description` frontmatter — that's the real OpenCode skill schema; don't add
a `triggers:` field, it isn't read by anything and just creates a second,
driftable source of truth alongside `description`.

| Skill | Coverage |
|-------|----------|
| `clion-tools` | CLion MCP tools for code search, debugging, building, diagnostics — use INSTEAD of grep/glob/bash |
| `memory-allocator` | Allocator selection, composition, arena patterns, poison API, STL adapter, safe_ptr |
| `module-architect` | Module naming, file structure, umbrella registration, CMake registration, pitfalls |
| `build-system` | Presets, setup_ppr_project, deps (CPM/vcpkg), MSVC workarounds, sanitizers |
| `hal-developer` | 10 areas across 4 platforms, syscall mapping, stub conventions, adding a platform |
| `concurrency-patterns` | RawChannel MPSC, IEvent/Signal, IContext tree, thread safety, HAL I/O integration |
| `code-reviewer` | Review against AGENTS.md conventions, C++ best practices, safe_ptr lifetime — needs `oracle` skill grant (see above) |
| `git-commit-planner` | Hunk-level analysis, cumulative-state rule, atomic commit ordering |
| `git-push-planner` | Squash planning, commit-message review, pre-push checklist |
| `git-log-fast-navigation` | Fast log formatting, rg-based commit/diff search, fzf browsing, .gitconfig aliases |
| `unit-test-updater` | Analyze diffs, update/add C++ unit tests |
| `validation` | Post-change checklist: compile every platform-relevant config, run engine tests, review diffs. Complements — doesn't replace — `deepwork`'s per-phase Oracle review gate; use `validation` standalone for a normal change, let `deepwork` invoke its own gate for multi-phase work |

Multi-phase refactoring and platform-porting work is handled by `deepwork`
(see [Risky / Multi-Phase Work](#risky--multi-phase-work)), not by a
standalone plan-execution skill — a custom top-level equivalent would just
duplicate its plan → Oracle-review → phased-execution loop.

## Available Commands

| Command | Coverage |
|---------|----------|
| `/review` | Parallel code review across 4 dimensions (security/memory, performance/cache, correctness/edge, conventions/style), fanned out to `oracle` subagents, correlated summary. Distinct from `@council`: same model family, different review lenses on the same diff. |

For architecture decisions or complex refactor proposals, use
`@council <question>` directly rather than a custom multi-proposal command —
Council already runs N independent models in parallel and synthesizes one
answer, with real provider diversity a single-model command can't match.

## Architecture Overview

Module dependency chain (entry point):
```
game/main.cpp → engine.app → engine.core  (foundation)
                            → engine.math  (vector math)
                            → engine.shader (shader compilation)
                            → engine.rhi   (GPU)
```

### engine.core — Foundation Library (`lib/engine/core/`)

31 module partitions providing all fundamental abstractions:

- **Types & Safety** (`Core.Types.cppm`): `u8`-`u64`/`i8`-`i64` shorthands, sentinel values (`default_value_v`, `zero_v`, `none_v`, `umax_v`), `Numeric<T,TagT>` strong wrapper, `hash_t`, `relocatable<T>` trait
- **Containers** (5 partitions): `Stack<T,N>`, `RingBuffer<T,N>`, `SparseVector<T>`, `StableVector<T>`, `HashMap<K,V>`/`HashSet<K>`, `FlatMap<K,V>`, `Bitmask<T,N>`, `ArrayView`, `RelativeView`, `TransformView`, `RelPtr`, `TagPtr`
- **Memory** (6 partitions): Allocator concepts (`TAllocator`, `TOwningAllocator`, `TBlockAllocator`, `TArenaAllocator`, `TSlabAllocator`), `GPA` (operator new), `OS` (page alloc), `PMR` (polymorphic dispatch), `HugePage` (2 MiB pools), `SmallPage` (32/64 KiB pools), `Arena`/`ScopedArena`/`ScratchPad` (TLS), `PagePool`/`LocalCache`/`HintedPooling`, composite allocators (`InSitu`, `Fallback`, `Threshold`, `Pooling`, `Static`), `Allocation<T,A>`, `Allocator<A>`, `STL<A>` adapter, poison/ASAN annotations
- **Concurrency** (3 partitions): `RawChannel` (lock-free MPSC), `IEvent`/`ISignal`/`Signal<Events...>` (compile-time event multiplexing), `IContext`/`SharedContext` (Go-style cancellation tree)
- **HAL** (`Core.HAL.cppm`): Platform abstraction over `pP::hal` — page memory, ring buffer, async I/O, file watching, process spawning, debugger, deadline timers, native string transcoding. Implemented per-platform in `lib/engine/core/hal/<platform>/` (windows, linux, darwin, generic — Windows has 13 files, Linux/Darwin/Generic have 10 each)
- **IO** (3 partitions): `hal::io` submit/poll/wait async I/O, memory-mapped files, directory watching
- **Services** (`Core.Service.cppm`): `IService` base with compile-time `typeUid<T>()` hash key, `ServicesStore` (thread-safe `FlatMap` with parent-chain fallback), `ServiceInjector` for implicit DI
- **Other**: `Logger`, `TimerManager`, `UnitTest` framework, `Callback<T>` with RAII Handle, `function_ref`, `Opaque` (variant/persistent/unique values and builder), string utilities, hashing infrastructure

### engine.math — Vector Math (`lib/engine/math/Math.cppm`)

Wraps `mango::math` into `namespace pP`:
- Type aliases: `float2/3/4`, `int2/3/4`, `uint2/3/4`, `float3x3`, `float4x4`
- Functions: `dot`, `dot2`, `lerp`, `normalize`, `distance`, `vector_cast`, `checked_cast` for vectors
- Matrix ops: `translate`, `scale`, `rotate`, `perspectiveD3D`, `lookAt`, `inverse`, `affineInverse`, `adjoint`, `oblique`
- Integration: `hashValue()` for hashing, `opaqueValue()` for serialization

### engine.rhi — GPU Abstraction (`lib/engine/rhi/RHI.cppm` + `RHI.cpp`)

Wraps Slang-RHI into `namespace pP::rhi`:
- Core types: `IDevice`, `IAdapter`, `IBuffer`, `ICommandBuffer`, `ICommandQueue`, `IRenderPipeline`, `IComputePipeline`, `IShaderProgram`, `ITexture`, `ITextureView`, `ISurface`, `IFence`, `IHeap`, `IInputLayout`
- Descriptors: `BufferDesc`, `DeviceDesc`, `RenderPipelineDesc`, `ShaderProgramDesc`, `SurfaceConfig`, etc.
- Projection helpers: `EProjectionConvention` (D3D/Vulkan depth conventions), `projectionConventionFromDeviceType()`, `getOrthoMatrix()`, `getPerspectiveMatrix()` — backend-aware matrix computation
- `IRhiService` interface — singleton service pattern wrapping `rhi::IRHI` and `rhi::IDevice` lifecycle; `createRenderPipeline()` virtual for pipeline creation from a render pass
- Implementation note: `App.Viewport.cppm`/`App.Viewport.cpp` is a partition pair — the `.cppm` holds declarations, the `.cpp` uses `module engine.app; import :viewport;` (never `module engine.app:viewport;`)

### engine.shader — Shader Compilation (`lib/engine/shader/Shader.cppm` + `Shader.cpp`)

Provides Slang shader compilation with hot-reload and background compilation:
- `IShaderService` interface — singleton service pattern wrapping `slang::Session` and `slang::Registry` lifecycle
- `ModuleHandle` — RAII wrapper for compiled shader modules
- File watching and hot-reload support via `hal::io`
- Background compile thread for async shader compilation
- Imported by `engine.rhi` (for `IShaderProgram` creation) and `engine.app` (for pipeline rebuild on reload)

### engine.app — Application Layer (`lib/engine/app/`, 24 partitions)

- **Application** (`App.Application.cppm`): Main loop class with virtual `initialize()`/`update()`/`render()`/`terminate()`, service store, per-frame timing, exit code management, directory resolution (install/config/content/working)
- **Input** (8 partitions): `IInputService` — keyboard/mouse/gamepad device states, listener stack (`pushInputListener`/`popInputListener`), action/mapping system (`InputMapping` binds keys to `InputAction` with `InputModifierEvent`/`InputTriggerEvent` callbacks), device enumeration
- **Window** (2 partitions): `IWindowService` — monitor enumeration, window creation/destruction/resize/move, event callbacks (`whenWindowResized`, `whenWindowFocused`, etc.)
- **Player** (2 partitions): `IPlayerService` — player identity management, graph-based state machine (`Player::Graph`), keyboard/gamepad player binding
- **Viewport** (1 partition, `renderer/App.Viewport.cppm`): multi-viewport render abstractions — `ViewportConfig` (plain data, `int2 framebuffer_size`), `ViewportEntry` (per-frame bundle: render pipeline + viewport + scissor + `function_ref` draw callback; NOT default-constructible — use designated aggregate init, and hoist draw lambdas into named variables: `function_ref` does not own its target), `EProjectionConvention`/`projectionConventionFromDeviceType` (D3D vs VK conventions), backend-aware projection helpers. Per-viewport isolation: `Application` keeps child `ServicesStore`s (e.g. `m_scene_services`/`m_ui_services`) chained to the root store — services registered there are visible only to their viewport, with parent-chain fallback
- **Platform**: GLFW backend (`platform/glfw/`, 9 files) implementing `IPlatform`, `IInputService`, `IPlayerService`, `IWindowService`
- **Renderer** (`App.Renderer.cppm`): `Renderer` class — `initialize(IRhiService, IWindowService, Window)` sets up pipeline, `render(span<const ViewportEntry>)` submits multi-viewport frames (viewport/scissor binding per entry via `RenderState`), backward-compat `render(optional<OverlayCallback>)` wrapper, `onResize()` handles surface resize

### Key Design Patterns

- **Service Locator**: `IService` → compile-time `typeUid<T>()` → `ServicesStore` with parent-chain walk → `ServiceInjector` for implicit dependency injection. Safe via `safe_ptr<T>` (debug: ref-counted lifetime check, release: raw pointer).
- **Allocator Composition**: Concepts tiered from `TAllocator` up to `TSlabAllocator`. Concrete allocators composed via `InSitu<T,N>` (inline storage), `Fallback<A,B>` (try A, then B), `Threshold<N,A,B>` (small→A, large→B), `Pooling<N,A>` (pool from A), `LocalCache<N,A,C>` (TLS cache over pool), `HintedPooling`. Wrap with `Allocator<A>` (type erasure), `PMR` (vtable dispatch), `STL<A>` (std:: adapter).
- **Event Multiplexing**: `IEvent` base → `Signal<Events...>` with compile-time composition and `std::counting_semaphore` → `select(events...)` Go-style helper for range-for over events.
- **Lock-free MPSC**: `RawChannel` for inter-thread message passing without mutex contention.
- **Cancellation Tree**: `IContext`/`SharedContext` — Go-style context propagation with deadline support.
- **Opaque Serialization**: `opaque::Value` (type-erased variant), `opaque::Block` (persistent byte buffer), `opaque::Unique` (RAII owning handle), `Block::Builder` (serialization builder).

### Test Infrastructure

Two separate test executables:
- `EngineCoreTests` (`lib/engine/tests/core/`) — GLFW-free; tests memory, containers, concurrency, IO, strings, utility, opaque, services, enums
- `EngineAppTests` (`lib/engine/tests/app/`) — links GLFW for platform-dependent tests

Shared in `lib/engine/tests/shared/` as static lib `EngineTestsShared` providing `parseCli()` and `runSuite()` to avoid duplication. Tests use `PPR_UNIT_TEST(name)` macros compiled as `inline constexpr` variables with `UnitTest` tree grouping, fork/crash support, and `--run-test --shuffle --loop` CLI. Test code includes the test-only header `"pP/UnitTest.h"` (from `lib/engine/tests/include/`, registered per test target) which provides `PPR_UNIT_TEST`, `PPR_TEST_ASSERT` (functional in release builds — always throws, unlike engine `PPR_ASSERT` which compiles to `[[assume]]`), and `PPR_UNIT_TEST_ERRC` (error-code opt-in bodies).

### Entry Point (`game/main.cpp`)

Imports all five engine modules, constructs `pP::Application(name, argv)`, calls `app.run()`. The application resolves install/config/content/working directories, discovers and initializes registered services (input, window, player, RHI, shader), then runs the per-frame update/render loop until exit.

## External File Loading
When you encounter a file reference (e.g., @rules/general.md), load it on demand.
Do NOT preemptively load all references. Treat loaded content as mandatory instructions.

## Tool Usage
Use tools in this priority order:
1. **CLion MCP tools** (`clion-*`) for code search, navigation, build, run, and debugging — platform-agnostic (identical on Windows/Linux/macOS), so prefer them over shell commands for search/listing/navigation; no pwsh/bash needed.
2. **Internal tools** (read/edit/grep/glob/task) for file and content operations — reading and editing known files, quick text search, subagent delegation.
3. **PowerShell (pwsh) only** for shell commands on Windows — never mix in other shells (Bash, cmd, Git Bash, etc.); use bash on Unix. `rg` (ripgrep) is an allowed exception for fast content search.

## Recon & Context (delegate to bundled skills first)

- **Repo map**: use `codemap` (`run codemap`) for hierarchical, change-detected
  `codemap.md` files instead of ad hoc project maps. It already handles
  incremental re-analysis of only changed folders — don't build a parallel
  cache for this.
- **Dependency internals** (Slang-RHI, mango::math, etc.): use `clonedeps`
  (`clone dependencies`). `orchestrator` asks `@librarian` to resolve the
  official repo/tag, confirms with you, then clones a pinned ref into
  `.slim/clonedeps/repos/` (max 3-5 deps, HTTPS + pinned refs only, no
  scripts run, kept out of git) and records it below.
- **Search scoping (default-exclude)**: mirror `.gitignore`. Exclude from all
  searches: `out/`, `_deps/`, `vcpkg_installed/`, `cmake-build-*/`, `build/`,
  `imgui_module_bindings`.
    - CLion MCP: pass `paths` excluding those dirs.
    - `rg` fallback: `rg --glob '!out/**' --glob '!**/vcpkg_installed/**' --glob '!**/_deps/**'`.
    - Once a dependency is cloned via `clonedeps`, read it from
      `.slim/clonedeps/repos/<name>/` directly rather than scoping into
      `vcpkg_installed`.

## Cloned Dependency Source

_(maintained by the `clonedeps` skill — empty until first run)_

## Risky / Multi-Phase Work

- **Multi-phase refactors, new HAL platforms, new module libraries**: start
  with `/deepwork <task>`. It creates a session artifact in
  `.slim/deepwork/<task>.md`, gets an Oracle review on the draft plan, splits
  it into phases (each with its own Oracle review), and executes phase by
  phase with validation between phases. PPR-specific mechanics — the module
  partition checklist, the CMake registration rule, the platform-file
  checklist below — are what `@fixer` follows *inside* each `deepwork` phase;
  they are not a competing top-level workflow.
- **Isolated lanes for risky or parallel work**: use `worktrees`
  (`work in a worktree`). Sets up `.slim/worktrees/<slug>/`, tracked in
  `.slim/worktrees.json`, with pre-flight dirty-tree checks and confirmation
  gates on every git mutation (`worktree add/remove`, `merge`, `rebase`,
  `cherry-pick`, `reset --hard`, branch ops). Do the actual implementation
  there, then hand off to `git-commit-planner`/`git-push-planner` for the
  atomic-commit and pre-push pass before `worktrees` integrates back.
- Keep `/review` (4-dimension parallel review) and Oracle's `deepwork`/`validation`
  gates for high-risk changes (memory, concurrency, build-system, new HAL
  platform).

### Session reuse (keyed + invalidated)
- Reuse a specialist session only when its **session key** matches: `(agent-type, target area, file-glob)`. MRU is a tiebreaker only.
- **Invalidate** sessions older than a threshold or whose key no longer matches the current task.
- **Never reuse mutating/debug sessions** (breakpoints, watches, state) — prefer fresh for debug; read-only recon sessions are safe to reuse.

### Targeted reads
- `read` with `offset`/`limit`; never dump whole large files.

### Tool priority (fallback ladder, not exclusive)
1. CLion MCP tools — `clion_skill_search` (unified file/text/regex/symbol search), `clion_list_directory_tree` (directory listing), `clion_search_symbol`, `clion_get_compiler_info` — language-aware, respects includes, platform-agnostic (no shell needed).
2. Internal `grep`/`glob` with exclusions.
3. `pwsh` + `rg` with exclusion globs (last resort).
- First `clion_search_*` after launch may be unindexed — tolerate.
- Canonical tool catalog lives in `clion-tools` SKILL.md; reference it, do not duplicate tool names here (avoid drift).

## Build System
- Load the `clion-tools` skill when starting any task. Follow the Tool Usage priority: CLion MCP tools for code search, building, and debugging; internal tools for file and content operations.
- CMake 4.3+, C++23, modules enabled, experimental `import std`.
- Presets: `msvc-dev` (recommended), `msvc-live` (Debug Edit&Continue, no ASAN), `msvc-rel`, `clang-cl-dev`, `clang-cl-rel`, `clang-dev`, `clang-rel`, `gcc-dev`/`gcc-rel` (hidden, no modules).
- Use `setup_ppr_project(Target INTERNAL_PUBLIC_DEPS ... EXTERNAL_SYSTEM_PRIVATE_DEPS ...)` for every target (see cmake/Compilers.cmake).
- Commit rule: new source file + its CMakeLists.txt registration go in the same commit.
- Two separate test executables: `EngineCoreTests` (core, GLFW-free) and `EngineAppTests` (links glfw). Aggregate target `run-engine-tests` runs both. Build via `cmake --build out/build/msvc-dev --target EngineCoreTests` (or `EngineAppTests`). Run via `run-engine-tests` run configuration in CLion.
- Shared test infrastructure in `lib/engine/tests/shared/` (static lib `EngineTestsShared`) provides `parseCli()` and `runSuite()` to avoid duplication between test executables.

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
- No raw loops (prefer algorithms/ranges). Comments should be exceptional — only add them for genuinely surprising or non-obvious code that cannot be clarified through naming or structure alone.
- `constexpr` everywhere, `[[nodiscard]]` on important returns, `noexcept` where possible.
- Inlining: `PPR_FORCE_INLINE` (hot paths), `PPR_NO_INLINE` (prevent), `PPR_FLATTEN` (recursive).
- Attributes: `PPR_EMPTY_BASES` (MSVC stateless wrappers), `PPR_LIFETIME_BOUND` (reference lifetime deps).
- Allowed macros: only those in `include/pP/Macros.h` — assertions (PPR_ASSERT/VERIFY/ENSURE/ASSUME), PPR_DEFER, inlining control, logging (PPR_LOG), and internal helper macros (stringize, concat, pragma, etc.). Test-only macros (`PPR_UNIT_TEST`, `PPR_TEST_ASSERT`, `PPR_UNIT_TEST_ERRC`) live in `lib/engine/tests/include/pP/UnitTest.h`, not in `Macros.h`. No macros from other sources.

## Type Safety
- `checked_cast<ToT>(v)` — safe narrowing/widening + downcast (dynamic_cast debug, static_cast release).
- `safe_narrowing<IntT>` — tag type asserting round-trip on implicit conversion.
- Integer shorthands: `u8/u16/u32/u64/i8/i16/i32/i64` (Core.Types.cppm).
- Sentinel values: `default_value_v`, `zero_v`, `none_v`, `umax_v`.
- `Numeric<T, TagT>` — strongly-typed numeric wrapper.
- `hash_t` — type-safe hash value (struct with m_value, comparison, hashValue).
- `pP::details::relocatable<T>` — mark types supporting memcpy.

## Assertions
| Macro | Debug Behavior | Release Behavior |
|-------|---------------|------------------|
| `PPR_ASSERT(expr)` | Calls onFailure, throws `std::logic_error` | `PPR_ASSUME(expr)` |
| `PPR_VERIFY(expr)` | Calls onFailure, throws `std::logic_error` | Evaluates expr + `PPR_ASSUME` |
| `PPR_ENSURE(expr)` | Returns false on failure | `PPR_ASSUME(expr)` then eval |
| `PPR_ASSUME(expr)` | `[[assume(expr)]]` / `__built_assume` | Same |
- `PPR_ENABLE_ASSERTIONS` = `PPR_ENABLE_DEBUG` (`_DEBUG` or `!NDEBUG`).
- `Assertion::setFailurePolicy()` lets tests intercept assertions.

## HAL
Platform code in `lib/engine/core/hal/<platform>/`. Supported: windows, linux, darwin, generic (stub). Selected via `PPR_HAL_PLATFORM` (cmake/HAL.cmake).
- `pP::hal`: pageAlloc/Free/Commit/Decommit/Protect/OfferToOS/ReclaimFromOS, ringBufferAlloc/Free, outputDebug, isDebuggerPresent, breakpoint.
- Sub-namespaces: `process` (executablePath, spawnAndWait, terminate), `timer` (setDeadline, cancelDeadline), `io` (async I/O, file watches), `native` (string transcoding).
- See `Core.HAL.cppm` for full API surface.

## Memory & Allocators
- Concepts: `TAllocator`, `TOwningAllocator`, `TResizableAllocator`, `TBlockAllocator`, `TArenaAllocator` (in Core.Memory.Allocator.cppm).
- Hierarchy: GPA (operator new) → OS (pageAlloc) → PagePool/BitmapTree → HugePage (2 MiB) / SmallPage (32/64 KiB) → Arena (persistent) / ScratchPad (TLS transient).
- Slab/Arena: Slab, InSituSlab, Arena<AllocatorT = HugePage>, ScopedArena (RAII watermark), ScratchPad (TLS Arena<SmallPage>).
- Composite: InSitu, Fallback, Threshold, Pooling, LocalCache, HintedPooling, Static, Allocation<T>, Allocator<>, AllocatorTraits, STL<>, Inplace.
- Poison: `poisonReserved`, `unpoisonUninitialized`, `poisonDestroyed`, `annotateContiguousContainer` (+ typed overloads). Uses `__asan_*` when ASAN enabled, debug patterns (0xAA/0xCC/0xDD) otherwise, no-op in release.
- For MSVC with `msvc-dev` preset, ASAN is auto-enabled via PPR_ENABLE_DEVELOPER_MODE.
- See `Core.Memory.*.cppm` for full type catalog.

## Core Abstractions
All types in `namespace pP`. See corresponding `.cppm` files:
- **Containers:** Stack<T,N>, RingBuffer<T,N> (bounded, trivial T); SparseVector<T>, StableVector<T>, HashMap<K,V>/HashSet<K>, FlatMap<K,V>, Bitmask<T,N>, SetBitsRange.
- **Pointers/views:** RelPtr (relative offset), TagPtr (flagged), ArrayView, RelativeView (half-size), safe_ptr, IndexIterator.
- **Strings:** string_literal, static_string<N>, char helpers (toLower, etc.), lazy transforms (caseFold, stringEscape, trim, etc.).
- **Opaque values:** opaque::Value (variant), opaque::Block (persistent), opaque::Unique (RAII owning), Block::Builder.
- **Concurrency:** IEvent/ISignal/Signal<...>, RawChannel (lock-free MPSC), IContext/SharedContext (Go-style cancellation).
- **Other:** IService/typeUid<T>, Log::Category/ELevel/Emitter, TimerManager, overloaded (visitor), std23::function_ref.

### safe_ptr<T>

- **Debug mode** (`PPR_ENABLE_DEBUG`): reference-counted lifetime checker — asserts that no `safe_ptr` outlives the pointed-to object
- **Release mode**: zero-overhead raw pointer (identical to `T*`)
- **NOT** a shared ownership pointer — the user guarantees init/destroy ordering
- All `safe_ptr` copies must be released (set to `nullptr` or go out of scope) before the owning object is destroyed
- `safe_ptr` from `unique_ptr::get()` is correct by design: the user guarantees the `unique_ptr` outlives all `safe_ptr` instances; `safe_ptr` will assert if violated

## Unit Testing
- Define: `PPR_UNIT_TEST(name) { PPR_TEST_ASSERT(cond); };` (tests are `inline constexpr` variables).
- `PPR_TEST_ASSERT` is functional in all build configs (throws `std::logic_error` via `onTestAssertionFailure`) — tests must never use `PPR_ASSERT`/`PPR_VERIFY`, which compile to `[[assume]]` in release. Test files include `"pP/UnitTest.h"` (test-only header; not part of the engine).
- Flags: `UnitTest::expect_fail` (must throw), `UnitTest::fork` (child process), `UnitTest::expect_crash` (fork + expect_fail).
- Group: `_.recurse({TestA, TestB, ...})` — supports conditional inclusion via `if constexpr (PPR_ENABLE_DEBUG)`.
- Module pattern: `export module engine.tests.core:memory;` with `export namespace pP::tests { ... }`.
- CLI: `EngineTests [--run-test <path>] [--shuffle [<seed>]] [--no-shuffle] [--loop <N>] [--child-run] [--help]` — test paths use `/` separators (e.g. `--run-test core/hal/thread_id`).
- Fork tests spawn child process via `hal::process::spawnAndWait`. Assertions intercepted by test framework (converted to failures, not terminations).
- Run programmatically: `pP::UnitTest::run(context, pP::tests::core);`.
- Optional error-code bodies: `PPR_UNIT_TEST_ERRC(name) { PPR_TEST_ASSERT_ERRC(cond); return {}; }` — the ec-reporting `run()` path (set `UnitTest::Context::m_fail_with` for message output).
- See `lib/engine/tests/` for existing examples.

## Debugging with CLion
- ALWAYS use CLion xdebug MCP tools for debugging. Never use printf/logging when the debugger is available.
- Workflow: start session → set breakpoint → resume → wait for pause → inspect stack/variables → step or continue.
- Load the `clion-tools` skill for the full tool reference and examples.
- Key tools: `clion_xdebug_start_debugger_session`, `clion_xdebug_set_breakpoint`, `clion_xdebug_control_session`, `clion_xdebug_get_stack`, `clion_xdebug_get_frame_values`, `clion_xdebug_evaluate_expression`.

## Matrix Layout Conventions

**PPR conventions (verified):**
- **Storage**: Row-major (HLSL/DirectX compatible), set explicitly in `lib/engine/shader/Shader.cpp` via `session_desc.defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_ROW_MAJOR;`
- **Vector–matrix multiply**: `mul(float4, matrix)` — row-vector convention (matrix on the right), matching `mango::math`. Do **not** use `mul(matrix, float4)`, which transposes the transform, flips view-space Z, and yields negative W (clipped geometry).
- **Camera / shader**: `m_view * m_projection` for viewProjection; shader does `mul(float4(input.position, 1.0), g_frame.m_view_projection)`.
- **RHI**: D3D vs VK projection Z-range handled via `EProjectionConvention`. No explicit `row_major`/`column_major` qualifiers in shaders (relies on HLSL default).

**Why row-major:** It is the only layout reliably portable across D3D, Vulkan, OpenGL, Metal, and CUDA. Slang's library API defaults to row-major (the `slangc` CLI defaults to column-major); PPR overrides it at the session level. Per-target layouts can be mixed via separate `SessionDesc`s.

**Handedness:** Slang-RHI does not enforce or override coordinate handedness (D3D traditionally left-handed, OpenGL/Vulkan right-handed, Metal varies) — that is the math library's responsibility. Configure the host math library to row-major and be explicit about vector interpretation for portability.

**Non-4x4 caveat:** Non-4x4 matrices (e.g. 4×3) may have size/alignment mismatches; insert a manual transpose at the host-to-shader boundary if sizes differ.

**References**
- Slang user guide: https://docs.shader-slang.org/en/stable/external/slang/docs/user-guide/a1-01-matrix-layout.html
- `lib/engine/shader/Shader.cpp` (rows 158-164, 192-198)

## CMake Version Tracking
- **CMake 4.4 synthetic target genex leak**: Single-config Ninja (CMAKE_BUILD_TYPE per preset) is used to avoid CMake 4.4's multi-config genex evaluation gap for C++ module synthetic targets. When CMake 4.5+ is adopted, test whether the `default` preset can switch back to `"Ninja Multi-Config"` without producing conflicting flags (e.g., `/Od` + `/Ox` in Debug synth targets). The `default` preset's description in CMakePresets.json contains a searchable reminder.
- **Root-scope module targets break `@cmake_cxx_std.lib` links**: CMake creates the synthetic `std` module target (`@cmake_cxx_std.lib`) in the directory scope of the first target that needs it (any target with `FILE_SET CXX_MODULES` when `CXX_MODULE_STD` is ON, regardless of whether its sources `import std;`). If that target lives in the TOP-LEVEL scope (e.g., a target defined via `include()`d cmake file rather than `add_subdirectory()`), the std library is referenced in link lines as the bare name `@cmake_cxx_std.lib` — the leading `@` is MSVC response-file syntax, so the linker drops it and every link fails with `LNK2001: unresolved external symbol std::_General_precision_tables_2<...>::_Max_P` (or similar std-module implicit-inline definitions). Fix: set `CXX_MODULE_STD OFF` on such targets (see `cmake/external/DearImGui.cmake`, where `ImGuiModule` triggered this via LNK2001 on `_Max_P`). When adopting a newer CMake, test whether root-scope module targets still produce a bare `@`-prefixed std lib reference.
