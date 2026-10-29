---
name: build-introspection
description: >
  Cost-efficient, evidence-first CMake build introspection for the PPR engine.
  Uses CMake File API replies from an existing configured build tree as the
  authoritative source for targets, module/source file sets, dependencies,
  toolchains, compiler flags, and optional timing artifacts. Prefer this skill
  when asked to inspect the current build graph without rebuilding.
---

# Build Introspection

Use this skill to answer build-structure questions from generated metadata, not
from `CMakeLists.txt` reconstruction.

## Contract

This skill reads CMake File API replies from an existing configured build tree,
using the requested build tree, or `out/build/clang-dev` on Linux and
`out/build/msvc-dev` on Windows. Pass `-BuildDir` explicitly; the helper may
retain a Windows default. It is authoritative for generated build
metadata and intentionally avoids scraping `CMakeLists.txt` as a substitute for
what CMake already emitted. It does not add dependencies, install runtime
hooks, or reconfigure/build unless the caller explicitly asks for a refresh.

Default output is terse text. Drill down only on the requested target or view.
Emit raw JSON only when explicitly requested.

## Evidence-first method

- **Evidence claim:** build facts reported by this skill come from CMake's
  generated File API replies in `<build>/.cmake/api/v1/reply/`.
- **Direct evidence path:** resolve the active build dir → find latest
  `index-*.json` → follow its `reply` object to `codemodel-v2`, `toolchains-v1`,
  `cache-v2`, and target JSON files → summarize only the requested facts.
- **Limits:** replies can be stale relative to source edits or missing if the
  build tree was never configured; File API only exposes fields CMake generated,
  so absent module/file-set detail must be reported as unavailable rather than
  inferred.
- **Verification affordance:** the helper script performs non-destructive path,
  index, and reply checks before reading; `-Json` exposes the exact structured
  data path used for a claim.

## Default build tree

- Default build dir: `out/build/clang-dev` on Linux; `out/build/msvc-dev` on Windows
- Default generator expectation in this repo: single-config Ninja
- Default stance: **read existing replies first**
- Reconfigure only when replies are missing/stale and the caller explicitly
  wants fresh metadata

This matches repo constraints from `AGENTS.md`: C++23/module-oriented builds,
the host Dev preset as the recommended preset, no `gcc-*` module validation path, and no
inventing source/module registration by reading `CMakeLists.txt` instead of the
generated graph.

## Query/reply lifecycle

1. Query markers live in `<build>/.cmake/api/v1/query/`.
2. CMake configure generates or refreshes `<build>/.cmake/api/v1/reply/`.
3. The newest `index-*.json` is the entry point; never guess reply filenames.
4. Follow the index to object replies:
   - `codemodel-v2` → targets, directories, projects, target json filenames
   - `toolchains-v1` → compiler identity/path/version per language
   - `cache-v2` → configured cache values when needed
   - `target-*.json` → sources, module file sets, compile/link/dependency facts
5. If a requested fact is absent from replies, report `not exposed by current
   File API reply`.

## Output contract

### Default: summary

Return a compact text block with only:
- build dir and index timestamp
- CMake version + generator
- configured projects/directories/targets count
- primary PPR targets detected (`engine.core`, `engine.math`, `engine.shader`,
  `engine.rhi`, `engine.image`, `engine.mesh`, `engine.sim`, `engine.physics`,
  `engine.app`, `engine.tests`, the test executables, `app.game`, `run-engine-tests`) when present
- staleness or missing-reply status

### Drilldown: target

For one target, return concise text with:
- target name, type, artifact path(s), source/build paths
- direct dependencies and interface link libraries, resolved to names where
  possible
- compile group fragments / language standard evidence as exposed
- source groups
- file sets and interface sources for C++ modules when exposed
- implementation sources count plus a short sample; use `-Json` for full lists

### Drilldown: compiler / deps / timing

- `compiler` → toolchain identity, path, version, language coverage
- `deps` → direct dependency edges for one target or a compact top-level target
  list when no target is specified
- `timing` → existing profiling/timing artifacts only; do not create them

### JSON mode

Only when explicitly requested. Return the structured object produced by the
helper script with no extra prose except status/error framing.

## Staleness and missing-reply rules

Report one of these states up front:

- `fresh-enough` — reply index exists and query markers are not newer than it;
  this checks query freshness only, not freshness relative to source or CMake edits
- `query-newer-than-reply` — at least one query marker is newer than the latest
  reply index; refresh requires explicit configure permission
- `missing-reply-index` — no `reply/index-*.json`
- `missing-object-reply` — index references a JSON file that is absent
- `unconfigured-build-tree` — build dir exists but File API reply set is absent
- `missing-build-tree` — build dir does not exist

Never silently fall back to `CMakeLists.txt` inspection to fill gaps.

## Safe PowerShell usage

Use PowerShell 7 (`pwsh`) on either host, from the repository root. Quote paths
with spaces. The examples below use the Windows build tree; on Linux pass
`out/build/clang-dev` explicitly. From Bash, for example:

```sh
pwsh -NoProfile -File .opencode/skills/build-introspection/Inspect-BuildFileApi.ps1 -BuildDir out/build/clang-dev -View compiler
```

### Default summary

```powershell
Test-Path -LiteralPath ".opencode/skills/build-introspection" ;
& ".\.opencode\skills\build-introspection\Inspect-BuildFileApi.ps1" -BuildDir "out/build/msvc-dev"
```

### Target drilldown

```powershell
Test-Path -LiteralPath "out/build/msvc-dev" ;
& ".\.opencode\skills\build-introspection\Inspect-BuildFileApi.ps1" -BuildDir "out/build/msvc-dev" -View target -Target "engine.core"
```

### Compiler configuration

```powershell
Test-Path -LiteralPath "out/build/msvc-dev/.cmake/api/v1/reply" ;
& ".\.opencode\skills\build-introspection\Inspect-BuildFileApi.ps1" -BuildDir "out/build/msvc-dev" -View compiler
```

### Raw JSON on request only

```powershell
& ".\.opencode\skills\build-introspection\Inspect-BuildFileApi.ps1" -BuildDir "out/build/msvc-dev" -View target -Target "engine.app" -Json
```

## Helper script

Use `Inspect-BuildFileApi.ps1` in this directory. It is read-only and resolves
the newest File API index automatically.

### Views

- `summary` (default)
- `target` (requires `-Target`)
- `compiler`
- `deps`
- `timing`

## Optional timing / instrumentation

When present, inspect existing artifacts only:
- File API reply timestamps
- CMake configure logs under `CMakeFiles/`
- profiling/trace files already emitted by an explicit CMake invocation
- `.ninja_log` as a build-time signal when the generator is Ninja

If those artifacts are absent, report `timing unavailable in current build tree`
instead of regenerating them.

## Refresh policy

Only with explicit caller approval:

```powershell
cmake --preset msvc-dev
```

The command above is Windows-specific; on Linux use `cmake --preset clang-dev`
in the environment defined by `build-system`. Use this only to refresh File API replies or when the build tree is missing.
Do not build targets just to answer introspection questions.

## Proportional verification for this skill

1. Validate path existence for the skill directory and helper script.
2. Validate script syntax by executing a non-destructive read-only view.
3. If `out/build/msvc-dev/.cmake/api/v1/reply/index-*.json` exists, run a dry
   summary and one target drilldown against that existing reply set.
4. Otherwise report clearly: `existing configured build tree required for live
   introspection; no configure performed by this skill validation`.

This verification stays read-only and does not reconfigure or build.
