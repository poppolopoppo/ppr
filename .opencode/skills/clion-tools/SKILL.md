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

> **Database tools intentionally not documented.** The CLion MCP server also
> exposes 14 database tools (SQL execution, connections, schema introspection).
> PPR engine work does not use them; they are omitted from this catalog to keep
> it focused. If you need them, call `clion_execute_tool` with the tool name to
> discover its signature, or consult JetBrains' CLion MCP documentation.

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
| `clion_get_file_problems` | ✅ YES | `filePath` |
| `clion_get_compiler_info` | ✅ YES | `filePath` |
| `clion_open_file_in_editor` | ✅ YES | `filePath` |
| `clion_get_diagnostic_info` | ✅ YES | (none beyond projectPath) |
| `clion_apply_patch` | ✅ YES | `input` |
| `clion_create_new_file` | ✅ YES | `pathInProject` |
| `clion_execute_tool` | ✅ YES | `command` |
| `clion_git_status` | ✅ YES | (none beyond projectPath) |
| `clion_get_repositories` | ✅ YES | (none beyond projectPath) |
| `clion_reformat_file` | ✅ YES | `files` |
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
code search and navigation, build/run integration, full debugger access, IDE
diagnostics, file editing, and VCS introspection. The orchestrator uses it to
decide *which* `clion_*` call to make, then delegates the call (and any
follow-up edits) to the appropriate subagent — the orchestrator does not invoke
MCP tools as a substitute for planning. The skill itself executes nothing; every
`clion_*` invocation is performed by the delegated subagent or background worker.

## Subagent routing

| Step | Delegate to | Why |
|------|-------------|-----|
| Symbol/file search, pre-flight recon | `@explorer` | Index-based search (`clion_search_symbol`, `clion_search_file`) |
| Diagnose errors/warnings, compiler config | `@oracle` | Root-cause judgment (`clion_get_file_problems`, `clion_get_compiler_info`, `clion_get_diagnostic_info`) |
| Bounded edits + debug-session driving | `@fixer` | Bounded implementation; `clion_xdebug_*` during fix application; `clion_apply_patch` / `clion_create_new_file` for edits |
| Build/test execution | background build subagent | Heavy, parallelizable (`clion_execute_run_configuration`, `clion_execute_terminal_command`) |
| Post-edit validation & hygiene | `@fixer` | `clion_get_file_problems` for inspection gate; `clion_reformat_file` for formatting |
| VCS recon | `@explorer` | `clion_git_status`, `clion_get_repositories` |
| Dynamic dispatch (unexposed tools) | `@explorer` or `@fixer` | `clion_execute_tool` as escape hatch |

## OMO feature wiring

- **Per-agent `skills`/`mcps` allow-lists** — the orchestrator already has
  `mcps: ["*","!context7"]` (includes `clion`); subagents should be scoped to
  only the `clion_*` tools they need: `@explorer` → `clion_search_symbol`,
  `clion_search_file`, `clion_git_status`, `clion_get_repositories` for pre-flight
  recon; `@oracle` → `clion_get_file_problems`, `clion_get_compiler_info`,
  `clion_get_diagnostic_info` for diagnosis; `@fixer` → `clion_xdebug_*` for
  bounded edits during fix application, plus `clion_apply_patch`,
  `clion_create_new_file`, `clion_reformat_file` for post-edit validation.
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

## 2. Dynamic Dispatch (`clion_execute_tool`)

Universal executor that invokes any IDE MCP tool dynamically. Use it as an
escape hatch when a tool you need is not directly exposed in your function list
(e.g., a newer plugin version added it, or you need to invoke a tool whose
direct binding is unavailable in this session).

**Syntax** (verified):
```
clion_execute_tool(command="tool_name --param value [--param2 value2 ...]", projectPath="E:/Code/ppr")
```

- Arguments are space-separated `--paramName value` pairs.
- Object/array parameters pass a JSON value: `--findings '[{...}]'`.
- The first token is the tool name; everything after is parsed as arguments.
- Quoted strings are supported for values containing spaces.

**Verified example:**
```
clion_execute_tool(command="search_symbol --q \"Application\" --limit 3", projectPath="E:/Code/ppr")
```

**Drift detector:** calling an unknown tool name via `clion_execute_tool`
returns an error message that lists the full available-tools registry. Use this
to verify the live catalog against this skill's documentation when in doubt.

## 3. Editing & Patching

### Apply a patch (`clion_apply_patch`)
Applies a patch using the Codex `apply_patch` format or unified git diff format.
Supports Add, Delete, and Update operations with optional Move-to-path for
updates. Paths must stay inside the project directory.

```
clion_apply_patch(input="*** Begin Patch\n*** Add File: lib/engine/foo.h\n+// new file\n*** End Patch", projectPath="E:/Code/ppr")
```

Use for structured multi-file edits from the IDE — keeps the IDE model in sync
with the working tree.

### Create a new file (`clion_create_new_file`)
Creates a new file at the specified path within the project directory and
optionally populates it with text. Creates any necessary parent directories
automatically.

```
clion_create_new_file(pathInProject="lib/engine/foo.h", text="// new file\n", projectPath="E:/Code/ppr")
```

Parameters: `pathInProject` (required), `text` (optional), `overwrite` (optional,
default false).

### Reformat files (`clion_reformat_file`)
Reformats the specified files in the JetBrains IDE using the project's code
formatting rules. Supports **batch** (multiple files in one call) and **line
ranges** (format only a portion of a single file).

```
clion_reformat_file(files=["lib/engine/core/Core.Memory.cppm"], projectPath="E:/Code/ppr")
clion_reformat_file(files=["lib/engine/core/Core.Memory.cppm"], startLine=42, endLine=80, projectPath="E:/Code/ppr")
```

Parameters: `files` (required, array of project-relative paths), `startLine`
(optional, 1-based inclusive), `endLine` (optional, 1-based inclusive),
`projectPath` (required).

Use after edits to normalize formatting; applies IDE formatting rules (the
project's `.clang-format` / IDE settings).

## 4. Building & Running

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

## 5. Debugging

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

### Run to a specific line
```
clion_xdebug_run_to_line(filePath="lib/engine/core/Core.Memory.cppm", line=150, projectPath="E:/Code/ppr")
```

### Typical debug workflow
1. `clion_xdebug_start_debugger_session(configurationName="EngineTests", projectPath="E:/Code/ppr")`
2. `clion_xdebug_set_breakpoint(filePath="...", line=N, projectPath="E:/Code/ppr")`
3. `clion_xdebug_control_session(action="RESUME", projectPath="E:/Code/ppr")` → `clion_xdebug_control_session(action="WAIT_FOR_PAUSE", projectPath="E:/Code/ppr")`
4. Inspect: `clion_xdebug_get_stack(projectPath="E:/Code/ppr")`, `clion_xdebug_get_frame_values(depth=2, projectPath="E:/Code/ppr")`
5. Step/evaluate as needed
6. `clion_xdebug_control_session(action="RESUME", projectPath="E:/Code/ppr")` to continue, or `action="STOP"` to end

## 6. Diagnostics

### Check a file for errors/warnings
```
clion_get_file_problems(filePath="lib/engine/core/Core.Memory.cppm", errorsOnly=false, projectPath="E:/Code/ppr")
```

Parameters: `filePath` (required, project-relative), `errorsOnly` (optional,
default false — set true to filter to errors only), `timeout` (optional),
`projectPath` (required).

Returns a list of problems with severity, description, and location (1-based
line/column). Use this as the inspection gate after edits: zero errors and
zero warnings on changed files is the pass criterion.

### Inspect compiler configuration
```
clion_get_compiler_info(filePath="lib/engine/core/Core.Memory.cppm", projectPath="E:/Code/ppr")
```

### Get full IDE diagnostic snapshot
```
clion_get_diagnostic_info(includeToolchains=true, includeBuildSystemWorkspaces=true, projectPath="E:/Code/ppr")
```

## 7. VCS

### List VCS roots
```
clion_get_repositories(projectPath="E:/Code/ppr")
```
Returns all VCS roots in the project — useful for multi-repository detection.

### Git status
```
clion_git_status(repositoryPathRelativeToProject="lib/engine", includeUntracked=true, includeIgnored=false, limit=50, projectPath="E:/Code/ppr")
```

Parameters: `repositoryPathRelativeToProject` (optional, filter to one repo),
`includeUntracked` (optional, default false), `includeIgnored` (optional,
default false), `limit` (optional, max entries per repo), `projectPath`
(required).

Returns porcelain-style index/worktree status codes and summary counters.

## 8. Open Files in IDE

```
clion_open_file_in_editor(filePath="lib/engine/core/Core.Memory.cppm", projectPath="E:/Code/ppr")
```

## Guidelines

- **Search:** Always use `clion_search_symbol` for finding types/functions. Use `clion_search_text` for content search. Use `clion_search_file` for file discovery.
- **Build:** Always use CLion run configurations instead of raw `cmake --build` bash commands when possible. After edits, validate with `clion_get_file_problems` on the touched files and reformat with `clion_reformat_file`.
- **Debug:** ALWAYS use `clion_xdebug_*` tools for debugging. Never use printf/logging for debugging when the debugger is available.
- **Diagnose:** Use `clion_get_file_problems` to check for errors before and after edits.
- **Edit:** Use `clion_apply_patch` for structured multi-file edits; `clion_create_new_file` for new files; `clion_reformat_file` (batch + line-range) for formatting.
- **VCS:** Use `clion_git_status` for precise changed-file enumeration; `clion_get_repositories` for multi-root detection.
- **Dynamic dispatch:** Use `clion_execute_tool` when a tool you need is not directly exposed in your function list. Unknown tool names dump the full registry — use as a drift detector.
- **Batch:** When multiple independent CLion calls are needed, batch them in parallel (e.g. `clion_search_symbol` + `clion_get_run_configurations` in the same message).
- **projectPath:** ALWAYS pass `projectPath="E:/Code/ppr"` in every `clion_*` call except `clion_get_all_open_file_paths`, `clion_xdebug_get_debugger_status`, and `clion_xdebug_list_breakpoints` (which auto-detect).
