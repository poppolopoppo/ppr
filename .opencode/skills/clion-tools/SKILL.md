---
name: clion-tools
description: >
  Use this skill whenever you need to search, navigate, build, debug, or diagnose
  code in the PPR project. CLion MCP tools are faster and more accurate than
  grep/glob/read for code search, provide proper build integration, and offer
  full debugger access with breakpoints, variable inspection, and expression
  evaluation. ALWAYS prefer CLion MCP tools over raw bash/grep alternatives.
---

# CLion MCP Tools

CLion exposes a rich MCP API. Prefer these tools over bash/grep/read equivalents
whenever available. The CLion instance must be running with the MCP plugin enabled.

## ⚠️ CRITICAL: The `projectPath` Parameter

**Every `clion_*` tool call MUST include `projectPath="E:/Code/ppr"`** — with very
few exceptions (see table below). The MCP server does NOT auto-detect the project
from the working directory, even though only one project is open. Calls without
`projectPath` fail with:

```
Unable to determine the target project for the current MCP tool call.
You may specify the project path via `projectPath` parameter when calling a tool.
```

### Why this happens

The CLion MCP server tracks multiple "open projects" internally. When you omit
`projectPath`, it cannot infer which one to target and aborts. Passing the
explicit path resolves it immediately.

### Which tools need `projectPath`

| Tool | Needs `projectPath`? | Required params (with projectPath) |
|------|---------------------|-----------------------------------|
| `clion_search_symbol` | ✅ YES | `q` |
| `clion_search_text` | ✅ YES | `q` |
| `clion_search_regex` | ✅ YES | `q` |
| `clion_search_file` | ✅ YES | `q` |
| `clion_list_directory_tree` | ✅ YES | `directoryPath` |
| `clion_read_file` | ✅ YES | `file_path` |
| `clion_get_run_configurations` | ✅ YES | `configurationName` OR `filePath`+`line` |
| `clion_execute_run_configuration` | ✅ YES | `configurationName` OR `filePath`+`line` |
| `clion_get_file_problems` | ✅ YES | `file_path` |
| `clion_get_compiler_info` | ✅ YES | `file_path` |
| `clion_open_file_in_editor` | ✅ YES | `file_path` |
| `clion_get_diagnostic_info` | ✅ YES | (none beyond projectPath) |
| `clion_xdebug_start_debugger_session` | ✅ YES | `configurationName` OR `filePath`+`line` |
| `clion_xdebug_set_breakpoint` | ✅ YES | `filePath`+`line` |
| `clion_xdebug_control_session` | ✅ YES | `action` |
| `clion_xdebug_get_stack` | ✅ YES | (session must exist) |
| `clion_xdebug_get_threads` | ✅ YES | (session must exist) |
| `clion_xdebug_get_frame_values` | ✅ YES | `frameIndex` |
| `clion_xdebug_get_value_by_path` | ✅ YES | `path` |
| `clion_xdebug_evaluate_expression` | ✅ YES | `expression` |
| `clion_xdebug_set_variable` | ✅ YES | `path`+`newValue` |
| `clion_xdebug_run_to_line` | ✅ YES | `filePath`+`line` |
| `clion_xdebug_remove_breakpoint` | ✅ YES | `breakpointId` OR `filePath`+`line` |
| `clion_xdebug_list_breakpoints` | ⚠️ AUTO | (none — auto-detects) |
| `clion_xdebug_get_debugger_status` | ⚠️ AUTO | (none — auto-detects) |
| `clion_get_all_open_file_paths` | ⚠️ AUTO | (none — auto-detects) |
| `clion_analyze_calls` | ✅ YES | `symbolFqn`+`analysisKind` |
| `clion_build_project` | ✅ YES | (none beyond projectPath) |
| `clion_get_project_dependencies` | ✅ YES | (none beyond projectPath) |
| `clion_get_project_modules` | ✅ YES | (none beyond projectPath) |
| `clion_lint_files` | ✅ YES | `files` |
| `clion_reformat_file` | ✅ YES | `path` |
| `clion_skill_search` | ✅ YES | `mode`+`q` |

**Rule of thumb:** If the tool touches project files, indexes, or build state,
pass `projectPath="E:/Code/ppr"`. Only the three debugger-state introspection
tools auto-detect.

### Correct vs. incorrect call

```jsonc
// ❌ WRONG — fails with "Unable to determine the target project"
clion_search_text(q="app_pixel_readback")

// ✅ CORRECT — works
clion_search_text(q="app_pixel_readback", projectPath="E:/Code/ppr")
```

**Agents: include `projectPath="E:/Code/ppr"` in EVERY `clion_*` call unless the
tool is one of the three auto-detecting ones above.**

## Contract

This skill is the catalog of CLion MCP tools for the PPR project: index-based
code search and navigation, build/run integration, full debugger access, and
IDE diagnostics. The orchestrator uses it to decide *which* `clion_*` call to
make, then delegates the call (and any follow-up edits) to the appropriate
subagent — the orchestrator does not invoke MCP tools as a substitute for
planning. The skill itself executes nothing; every `clion_*` invocation is
performed by the delegated subagent or background worker.

## Subagent routing

| Step | Delegate to | Why |
|------|-------------|-----|
| Symbol/file search, pre-flight recon | `@explorer` | Index-based search (`clion_search_symbol`, `clion_search_file`) |
| Call hierarchy analysis | `@oracle` | Call-graph-based architecture review (`clion_analyze_calls`) |
| Diagnose errors/warnings, compiler config | `@oracle` | Root-cause judgment (`clion_get_file_problems`, `clion_get_compiler_info`, `clion_get_diagnostic_info`) |
| Bounded edits + debug-session driving | `@fixer` | Bounded implementation; `clion_xdebug_*` during fix application |
| Build/test execution | background build subagent | Heavy, parallelizable (`clion_execute_run_configuration`, `clion_execute_terminal_command`) |
| Post-edit validation & hygiene | `@fixer` | `clion_build_project`, `clion_lint_files`, `clion_reformat_file` |
| Code search with mode dispatch | `@explorer` | `clion_skill_search` for unified search |

## OMO feature wiring

- **Per-agent `skills`/`mcps` allow-lists** — the orchestrator already has
  `mcps: ["*","!context7"]` (includes `clion`); subagents should be scoped to
  only the `clion_*` tools they need: `@explorer` → `clion_search_symbol`,
  `clion_search_file` for pre-flight recon; `@oracle` →
  `clion_get_file_problems`, `clion_get_compiler_info`,
  `clion_get_diagnostic_info` for diagnosis; `@fixer` → `clion_xdebug_*` for
  bounded edits during fix application, plus `clion_build_project`,
  `clion_lint_files`, `clion_reformat_file` for post-edit validation.
- **Background orchestration** — run configurations execute as background
  subagents with appropriate timeouts. `EngineTests` configuration: 120s
  timeout default; `EngineAppTests` may require longer for GLFW-dependent
  builds. The orchestrator queues build/test operations and surfaces results
  without blocking the main workflow.
- **Session reuse** — cache run-configuration names across prompts to avoid
  redundant discovery. Reuse debug sessions across `--run-test` prompts when
  the same configuration and test path apply. Debug session state
  (breakpoints, variable states) is transient — reset between independent
  debugging contexts.
- **`orchestratorPrompt` routing** — trigger on "search the codebase",
  "build", "debug", "run EngineTests". The orchestrator matches the user
  intent to the appropriate CLion tool category and delegates accordingly.

## 1. Code Search & Navigation

These are faster and more accurate than grep/glob because they use CLion's
index (semantic understanding, not just text matching).

**All calls below require `projectPath="E:/Code/ppr"` — shown once for brevity,
include it in every call.**

### Find by symbol name (types, functions, classes)
```
clion_search_symbol(q="YourClass", projectPath="E:/Code/ppr")
clion_search_symbol(q="functionName", include_external=true, projectPath="E:/Code/ppr")  # include SDK symbols
```

### Find by text content
```
clion_search_text(q="exact substring", paths=["src/**"], projectPath="E:/Code/ppr")
```

### Find by regex
```
clion_search_regex(q="\\bfunc\\w+\\(", paths=["lib/engine/**"], projectPath="E:/Code/ppr")
```

### Find files
```
clion_search_file(q="Core.*.cppm", projectPath="E:/Code/ppr")
```

### Call hierarchy analysis
Builds the IDE Call Hierarchy tree for a method, function, constructor, or supported type target.

- Use **INCOMING_CALLS** to see who calls a symbol.
- Use **OUTGOING_CALLS** to see what the symbol calls.
- Prefer it over usage/text/regex search when evaluating dependencies by actual calls — it uses IDE call hierarchy data, giving more precise call relationships with less noise.

```
clion_analyze_calls(symbolFqn="pP::HashMap::insert", analysisKind="OUTGOING_CALLS", projectPath="E:/Code/ppr")
clion_analyze_calls(symbolFqn="pP::Application::run", analysisKind="INCOMING_CALLS", depth=3, projectPath="E:/Code/ppr")
```

**Parameters:**
- `symbolFqn` (required) — fully qualified name like `pP::HashMap::insert`; if ambiguous, the tool returns exact signatures to pass back; if only a short name is known, use `clion_search_symbol` first.
- `analysisKind` (required) — `INCOMING_CALLS` or `OUTGOING_CALLS`.
- `depth` (default 5) — max call levels below subtree root; 0 = root only.
- `maxChildren` (default 50) — max children per node.
- `maxNodes` (default 1000) — max total nodes in the tree.
- `treePath` (optional) — subtree root path copied exactly from a previous result, for expanding the tree.
- `childOffset` (optional) — paging direct children; use after a truncated `... and n more` line.
- `timeout` — optional timeout in milliseconds.
- `projectPath` (required) — `E:/Code/ppr`.

**Result:** An expandable text tree; each node has `filePath` and `treePath`. Pass `treePath` back to render a subtree; use `childOffset` to continue after a truncated `... and n more` line.

**When to prefer:** Use `clion_analyze_calls` (INCOMING_CALLS/OUTGOING_CALLS) instead of text/regex search when the question is "who calls this / what does this call".

### Unified search
Performs a unified project search with an explicit mode: `file` (glob path search), `text` (literal content search), `regex` (regex content search), `symbol` (semantic symbol lookup).

- Symbol search is project-focused by default; retry with `include_external=true` to search SDK/library symbols.
- Position it as the one-stop search entry point that dispatches to the right search kind via `mode`.

**Modes:**
- `file` — glob path search; `q` is a glob pattern; `paths` are project-relative glob filters (supports `!`-excludes and trailing `/`); `includeExcluded` controls excluded files.
- `text` — literal content search; `q` is the exact substring to find.
- `regex` — regex content search; `q` is the regex pattern.
- `symbol` — semantic symbol lookup; `q` is the symbol name; `include_external` controls SDK/library symbol inclusion.

```
clion_skill_search(mode="symbol", q="HashMap", projectPath="E:/Code/ppr")
clion_skill_search(mode="file", q="Core.*.cppm", paths=["lib/engine/**"], projectPath="E:/Code/ppr")
clion_skill_search(mode="text", q="pixel_readback", projectPath="E:/Code/ppr")
clion_skill_search(mode="regex", q="\\bfunction\\b", projectPath="E:/Code/ppr")
```

**Parameters:**
- `mode` (required) — one of `file`, `text`, `regex`, `symbol`.
- `q` (required) — the search term; meaning depends on `mode`.
- `paths` (optional) — project-relative glob filters, supports `!`-excludes and trailing `/`.
- `include_external` (symbol mode only) — include SDK/library symbols.
- `includeExcluded` (file mode only) — include excluded files.
- `limit` — max results to return.
- `projectPath` (required) — `E:/Code/ppr`.

### Navigate project structure
```
clion_list_directory_tree(directoryPath="lib/engine", maxDepth=2, projectPath="E:/Code/ppr")
clion_get_all_open_file_paths()  # auto-detects project — no projectPath needed
```

### Read files
```
clion_read_file(file_path="lib/engine/core/Core.Memory.Allocator.cppm", offset=1, limit=200, projectPath="E:/Code/ppr")
```

**When to prefer CLion tools over grep/read:**
- Searching for a type/function definition → `clion_search_symbol`
- Finding where a string appears → `clion_search_text` (uses IntelliJ index, handles modules)
- Locating files by name pattern → `clion_search_file`
- Browsing directory structure → `clion_list_directory_tree`
- Reading a known file → `clion_read_file`

Note: the first `clion_search_*` after CLion launch may be unindexed — tolerate
the slower first response rather than falling back to grep.

## 2. Building & Running

### Discover available run configurations
```
clion_get_run_configurations(projectPath="E:/Code/ppr")
```
Returns list of configurations with names, targets, working directories, and whether
they support dynamic launch overrides.

### Execute a run configuration
```
clion_execute_run_configuration(configurationName="EngineTests", timeout=120000, waitForExit=true, projectPath="E:/Code/ppr")
```

### Execute with overrides (one-time)
```
clion_execute_run_configuration(
  configurationName="EngineTests",
  programArguments="--run-test core.memory",
  envs={"MY_VAR": "value"},
  timeout=60000,
  waitForExit=true,
  projectPath="E:/Code/ppr"
)
```

### Build the project
Triggers building of the project or specified files, waits for completion, returns build errors.

- **MUST be used after performing edits to validate the edits are valid.**
- Parameters: `rebuild` (full rebuild, default `false`; effective only when `filesToRebuild` is not specified), `filesToRebuild` (only compile files with the specified paths, relative to project root), `timeout`, `projectPath`.

**Examples:**
- Full project build:
```
clion_build_project(rebuild=true, projectPath="E:/Code/ppr")
```
- Build only the changed files after an edit:
```
clion_build_project(filesToRebuild=["lib/engine/core/Core.Memory.cppm"], projectPath="E:/Code/ppr")
```

**When to prefer:** Use `clion_build_project` after any edit to confirm the build succeeds. Use `filesToRebuild` to limit the build to only the files you changed, speeding up the validation cycle.

### Project introspection
Returns structured project metadata for build-system recon before planning changes that touch CMake/dependencies.

- `clion_get_project_dependencies`: returns a list of all dependencies defined in the project, with structured info about library names.
- `clion_get_project_modules`: returns a list of all modules in the project with their types (name + type).

**Examples:**
- List all project dependencies:
```
clion_get_project_dependencies(projectPath="E:/Code/ppr")
```
- List all project modules:
```
clion_get_project_modules(projectPath="E:/Code/ppr")
```
```

### Run from code location
```
clion_get_run_configurations(filePath="lib/engine/tests/core/Core.Memory.Tests.cppm", projectPath="E:/Code/ppr")
clion_execute_run_configuration(filePath="lib/engine/tests/core/Core.Memory.Tests.cppm", line=42, projectPath="E:/Code/ppr")
```

### Run terminal commands
```
clion_execute_terminal_command(command="cmake --build --preset msvc-dev --target EngineCoreTests", projectPath="E:/Code/ppr")
```

**When to prefer CLion over bash:**
- Building a target → use `clion_execute_run_configuration` with a run config, or `clion_execute_terminal_command`
- Running tests → `clion_execute_run_configuration(configurationName="EngineTests", projectPath="E:/Code/ppr")`
- Checking build results → `clion_execute_run_configuration` returns output + exit code

## 3. Debugging

Full debugger access: breakpoints, stepping, variable inspection, expression evaluation.

**All `clion_xdebug_*` calls require `projectPath="E:/Code/ppr"` EXCEPT
`clion_xdebug_get_debugger_status` and `clion_xdebug_list_breakpoints`, which
auto-detect the project.**

### Start a debug session
```
clion_xdebug_start_debugger_session(configurationName="EngineTests", projectPath="E:/Code/ppr")
```
Or from a code location:
```
clion_xdebug_start_debugger_session(filePath="lib/engine/tests/core/Core.Memory.Tests.cppm", line=42, projectPath="E:/Code/ppr")
```

### Set breakpoints
```
clion_xdebug_set_breakpoint(filePath="lib/engine/core/Core.Memory.cppm", line=100, projectPath="E:/Code/ppr")
```

Conditional breakpoint:
```
clion_xdebug_set_breakpoint(
  filePath="lib/engine/core/Core.Memory.cppm",
  line=100,
  condition="size > 1024",
  projectPath="E:/Code/ppr"
)
```

Temporary breakpoint (removed after first hit):
```
clion_xdebug_set_breakpoint(
  filePath="lib/engine/core/Core.Memory.cppm",
  line=100,
  temporary=true,
  projectPath="E:/Code/ppr"
)
```

Log breakpoint (doesn't suspend, just logs):
```
clion_xdebug_set_breakpoint(
  filePath="lib/engine/core/Core.Memory.cppm",
  line=100,
  isLogMessage=true,
  isLogStack=true,
  suspendPolicy="NONE",
  projectPath="E:/Code/ppr"
)
```

### List & remove breakpoints
```
clion_xdebug_list_breakpoints()  # auto-detects — no projectPath needed
clion_xdebug_list_breakpoints(filePath="lib/engine/core/Core.Memory.cppm", projectPath="E:/Code/ppr")
clion_xdebug_remove_breakpoint(breakpointId="<id>", projectPath="E:/Code/ppr")
```

### Control execution
```
clion_xdebug_control_session(action="RESUME", projectPath="E:/Code/ppr")
clion_xdebug_control_session(action="STEP_INTO", projectPath="E:/Code/ppr")
clion_xdebug_control_session(action="STEP_OVER", projectPath="E:/Code/ppr")
clion_xdebug_control_session(action="STEP_OUT", projectPath="E:/Code/ppr")
clion_xdebug_control_session(action="PAUSE", projectPath="E:/Code/ppr")
clion_xdebug_control_session(action="STOP", projectPath="E:/Code/ppr")
clion_xdebug_control_session(action="WAIT_FOR_PAUSE", timeout=30000, projectPath="E:/Code/ppr")
```

### Inspect state when paused
```
clion_xdebug_get_stack(projectPath="E:/Code/ppr")                           # call stack
clion_xdebug_get_threads(projectPath="E:/Code/ppr")                         # all threads
clion_xdebug_get_frame_values(depth=2, projectPath="E:/Code/ppr")          # variables in current frame
clion_xdebug_get_value_by_path(path=["myObject", "field", "subField"], projectPath="E:/Code/ppr")  # drill into nested fields
clion_xdebug_evaluate_expression(expression="myVector.size()", projectPath="E:/Code/ppr")  # evaluate arbitrary expression
```

### Mutate state at runtime
```
clion_xdebug_set_variable(path=["myVar"], newValue="42", projectPath="E:/Code/ppr")
```

### Typical debug workflow
1. `clion_xdebug_start_debugger_session(configurationName="EngineTests", projectPath="E:/Code/ppr")`
2. `clion_xdebug_set_breakpoint(filePath="...", line=N, projectPath="E:/Code/ppr")`
3. `clion_xdebug_control_session(action="RESUME", projectPath="E:/Code/ppr")` → `clion_xdebug_control_session(action="WAIT_FOR_PAUSE", projectPath="E:/Code/ppr")`
4. Inspect: `clion_xdebug_get_stack(projectPath="E:/Code/ppr")`, `clion_xdebug_get_frame_values(depth=2, projectPath="E:/Code/ppr")`
5. Step/evaluate as needed
6. `clion_xdebug_control_session(action="RESUME", projectPath="E:/Code/ppr")` to continue, or `action="STOP"` to end

## 4. Diagnostics

### Check a file for errors/warnings
```
clion_get_file_problems(filePath="lib/engine/core/Core.Memory.cppm", errorsOnly=true, projectPath="E:/Code/ppr")
```

### Inspect compiler configuration
```
clion_get_compiler_info(filePath="lib/engine/core/Core.Memory.cppm", projectPath="E:/Code/ppr")
```

### Get full IDE diagnostic snapshot
```
clion_get_diagnostic_info(includeToolchains=true, includeBuildSystemWorkspaces=true, projectPath="E:/Code/ppr")
```

### Lint multiple files
Analyzes the specified files for errors and warnings using IntelliJ inspections; use it to lint several files after editing them.

- Batch responses may include `timedOut: true` entries when individual files exceed the budget; `notAnalyzedReason` marks files that could not be analyzed (outside content roots, excluded, unsupported types); top-level `more: true` means the batch is incomplete.
- Contrast with `clion_get_file_problems` (single file) — `lint_files` is the batch version.

```
clion_lint_files(files=["lib/engine/core/Core.Memory.cppm", "lib/engine/math/Math.cppm"], min_severity="warning", projectPath="E:/Code/ppr")
```

## 5. Open Files in IDE
```
clion_open_file_in_editor(filePath="lib/engine/core/Core.Memory.cppm", projectPath="E:/Code/ppr")
```

### Reformat a file
Reformats the specified file in the JetBrains IDE using the project's code formatting rules.

- Use after edits to normalize formatting; applies IDE formatting rules (the project's `.clang-format` / IDE settings).

```
clion_reformat_file(path="lib/engine/core/Core.Memory.cppm", projectPath="E:/Code/ppr")
```

## Guidelines

- **Search:** Always use `clion_search_symbol` for finding types/functions. Use `clion_search_text` for content search. Use `clion_search_file` for file discovery. Use `clion_skill_search` with the appropriate `mode` as the unified search entry point.
- **Build:** Always use CLion run configurations instead of raw `cmake --build` bash commands when possible. After edits, validate with `clion_build_project` (or `filesToRebuild` for the touched files) and lint with `clion_lint_files`; reformat edited files with `clion_reformat_file`.
- **Debug:** ALWAYS use `clion_xdebug_*` tools for debugging. Never use printf/logging for debugging when the debugger is available.
- **Diagnose:** Use `clion_get_file_problems` to check for errors before and after edits.
- **Call hierarchy:** Use `clion_analyze_calls` (INCOMING_CALLS/OUTGOING_CALLS) instead of text/regex search when the question is "who calls this / what does this call".
- **Batch:** When multiple independent CLion calls are needed, batch them in parallel (e.g. `clion_search_symbol` + `clion_get_run_configurations` in the same message).
- **projectPath:** ALWAYS pass `projectPath="E:/Code/ppr"` in every `clion_*` call except `clion_get_all_open_file_paths`, `clion_xdebug_get_debugger_status`, and `clion_xdebug_list_breakpoints` (which auto-detect).
