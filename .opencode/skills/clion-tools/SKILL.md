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

## Tool Availability

All tools below are prefixed `clion_`. They are available when CLion is running
and connected via MCP (`opencode.json` → `mcp.clion`).

## 1. Code Search & Navigation

These are faster and more accurate than grep/glob because they use CLion's
index (semantic understanding, not just text matching).

### Find by symbol name (types, functions, classes)
```
clion_search_symbol(q="YourClass")
clion_search_symbol(q="functionName", include_external=true)  # include SDK symbols
```

### Find by text content
```
clion_search_text(q="exact substring", paths=["src/**"])
```

### Find by regex
```
clion_search_regex(q="\\bfunc\\w+\\(", paths=["lib/engine/**"])
clion_search_in_files_by_regex(regexPattern="TODO|FIXME", fileMask="*.cppm")
```

### Find files
```
clion_find_files_by_name_keyword(nameKeyword="Memory")
clion_find_files_by_glob(globPattern="**/*.cppm")
clion_search_file(q="Core.*.cppm")
```

### Navigate project structure
```
clion_list_directory_tree(directoryPath="lib/engine", maxDepth=2)
clion_get_all_open_file_paths()
```

### Read files
```
clion_read_file(file_path="lib/engine/core/Core.Memory.Allocator.cppm", offset=1, limit=200)
clion_get_file_text_by_path(pathInProject="lib/engine/core/Core.Memory.Allocator.cppm")
```

**When to prefer CLion tools over grep/read:**
- Searching for a type/function definition → `clion_search_symbol`
- Finding where a string appears → `clion_search_text` (uses IntelliJ index, handles modules)
- Locating files by name pattern → `clion_search_file` or `clion_find_files_by_name_keyword`
- Browsing directory structure → `clion_list_directory_tree`
- Reading a known file → `clion_read_file` or `clion_get_file_text_by_path`

## 2. Building & Running

### Discover available run configurations
```
clion_get_run_configurations()
```
Returns list of configurations with names, targets, working directories, and whether
they support dynamic launch overrides.

### Execute a run configuration
```
clion_execute_run_configuration(configurationName="EngineTests", timeout=120000, waitForExit=true)
```

### Execute with overrides (one-time)
```
clion_execute_run_configuration(
  configurationName="EngineTests",
  programArguments="--run-test core.memory",
  envs={"MY_VAR": "value"},
  timeout=60000,
  waitForExit=true
)
```

### Run from code location
```
clion_get_run_configurations(filePath="lib/engine/tests/Core.Memory.cppm")
clion_execute_run_configuration(filePath="lib/engine/tests/Core.Memory.cppm", line=42)
```

### Run terminal commands
```
clion_execute_terminal_command(command="cmake --build --preset msvc-dev --target EngineTests")
```

**When to prefer CLion over bash:**
- Building a target → use `clion_execute_run_configuration` with a run config, or `clion_execute_terminal_command`
- Running tests → `clion_execute_run_configuration(configurationName="EngineTests")`
- Checking build results → `clion_execute_run_configuration` returns output + exit code

## 3. Debugging

Full debugger access: breakpoints, stepping, variable inspection, expression evaluation.

### Start a debug session
```
clion_xdebug_start_debugger_session(configurationName="EngineTests")
```
Or from a code location:
```
clion_xdebug_start_debugger_session(filePath="lib/engine/tests/Core.Memory.cppm", line=42)
```

### Set breakpoints
```
clion_xdebug_set_breakpoint(filePath="lib/engine/core/Core.Memory.cppm", line=100)
```

Conditional breakpoint:
```
clion_xdebug_set_breakpoint(
  filePath="lib/engine/core/Core.Memory.cppm",
  line=100,
  condition="size > 1024"
)
```

Temporary breakpoint (removed after first hit):
```
clion_xdebug_set_breakpoint(
  filePath="lib/engine/core/Core.Memory.cppm",
  line=100,
  temporary=true
)
```

Log breakpoint (doesn't suspend, just logs):
```
clion_xdebug_set_breakpoint(
  filePath="lib/engine/core/Core.Memory.cppm",
  line=100,
  isLogMessage=true,
  isLogStack=true,
  suspendPolicy="NONE"
)
```

### List & remove breakpoints
```
clion_xdebug_list_breakpoints()
clion_xdebug_list_breakpoints(filePath="lib/engine/core/Core.Memory.cppm")
clion_xdebug_remove_breakpoint(breakpointId="<id>")
```

### Control execution
```
clion_xdebug_control_session(action="RESUME")
clion_xdebug_control_session(action="STEP_INTO")
clion_xdebug_control_session(action="STEP_OVER")
clion_xdebug_control_session(action="STEP_OUT")
clion_xdebug_control_session(action="PAUSE")
clion_xdebug_control_session(action="STOP")
clion_xdebug_control_session(action="WAIT_FOR_PAUSE", timeout=30000)
```

### Inspect state when paused
```
clion_xdebug_get_stack()                           # call stack
clion_xdebug_get_threads()                         # all threads
clion_xdebug_get_frame_values(depth=2)             # variables in current frame
clion_xdebug_get_value_by_path(path=["myObject", "field", "subField"])  # drill into nested fields
clion_xdebug_evaluate_expression(expression="myVector.size()")  # evaluate arbitrary expression
```

### Mutate state at runtime
```
clion_xdebug_set_variable(path=["myVar"], newValue="42")
```

### Typical debug workflow
1. `clion_xdebug_start_debugger_session(configurationName="EngineTests")`
2. `clion_xdebug_set_breakpoint(filePath="...", line=N)`
3. `clion_xdebug_control_session(action="RESUME")` → `clion_xdebug_control_session(action="WAIT_FOR_PAUSE")`
4. Inspect: `clion_xdebug_get_stack()`, `clion_xdebug_get_frame_values(depth=2)`
5. Step/evaluate as needed
6. `clion_xdebug_control_session(action="RESUME")` to continue, or `action="STOP"` to end

## 4. Diagnostics

### Check a file for errors/warnings
```
clion_get_file_problems(filePath="lib/engine/core/Core.Memory.cppm", errorsOnly=true)
```

### Inspect compiler configuration
```
clion_get_compiler_info(filePath="lib/engine/core/Core.Memory.cppm")
```

### Get full IDE diagnostic snapshot
```
clion_get_diagnostic_info(includeToolchains=true, includeBuildSystemWorkspaces=true)
```

### Inspect database schema (if applicable)
```
clion_list_database_connections()
clion_list_database_schemas(connectionId="...", selectedOnly=false)
clion_get_database_object_description(connectionId="...", databaseName="...", schemaName="...", kind="table", objectName="...")
```

## 5. Open Files in IDE
```
clion_open_file_in_editor(filePath="lib/engine/core/Core.Memory.cppm")
```

## Guidelines

- **Search:** Always use `clion_search_symbol` for finding types/functions. Use `clion_search_text` for content search. Use `clion_find_files_by_*` for file discovery.
- **Build:** Always use CLion run configurations instead of raw `cmake --build` bash commands when possible.
- **Debug:** ALWAYS use `clion_xdebug_*` tools for debugging. Never use printf/logging for debugging when the debugger is available.
- **Diagnose:** Use `clion_get_file_problems` to check for errors before and after edits.
- **Batch:** When multiple independent CLion calls are needed, batch them in parallel (e.g. `clion_search_symbol` + `clion_get_run_configurations` in the same message).
