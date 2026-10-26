---
name: pix-profiling
description: Capture and analyze PPR CPU timing scopes and app.game DirectX 12 work through the configured PozLabs pix-mcp server, including sampling-free timing, GPU triage, and bounded comparisons.
---

# PIX Profiling

Use the connected PozLabs `pix-mcp` server. `opencode.json` under `mcp.servers`
describes the repository's OpenCode registration; another client's live connection
must be verified separately. The upstream
[README](https://github.com/PozLabs/pix-mcp/blob/main/README.md) and
[SETUP](https://github.com/PozLabs/pix-mcp/blob/main/SETUP.md) own server capabilities
and setup details. Discover the installed tool schemas before calling them;
the connected executable may differ from upstream or the repository pin.

For a new capture workflow, read the
[execution-tools runbook](../../../docs/debugging/execution-tools.md) first.
It records the October 2026 PID-discovery/deadline failures and distinguishes
verified capture evidence from server readiness. Do not repeat an expensive
workload merely to rediscover a known orchestration failure.

Coverage policy lives in [AGENTS.md](../../../AGENTS.md#runtime-cost); the
[profiling guide](../../../docs/profiling.md) explains HAL scope placement and
interpretation. HAL helpers emit CPU events from inside the target process.
An external capture tool cannot insert those scopes or turn them into GPU timings.

## Prepare a reproducible workload

1. Use `build-system` for the workload's build when needed; `app.game` uses
   `msvc-profile` for Release markers. Retain the exact executable path/hash,
   matching PDB identity when present, compiled-source snapshot, and effective
   build/configuration. Keep later working-tree edits separate from that build identity.
   Tests need explicit `--loop 1` and the current harness's no-shuffle control;
   verify their supported syntax. Measure the focused workload's runtime before
   choosing a bounded interval and completion allowance. Do not inherit a long
   suite timeout or impose a benchmark when the task only needs trace inspection.
2. Call `pix_status` on the actual connection. Check its readiness and
   admin/elevation fields for the chosen capture mode, then discover live schemas.
   Do not assume status exposes commands, effective settings, or Developer Mode
   readiness. Windows Developer Mode is required for PIX replay/analysis verbs,
   not for the supported local read-only SQL inspection described below.
3. Choose a repeatable scene, camera, edit workload, warmup, and capture point.
   Record effective display/focus settings. Game arguments/configuration are
   resolved by `game/Game.Config.*`, not a hand-written loop in `main.cpp`.
   Profiling defaults can disable unfocused throttling, but explicit settings
   can override them; verify the effective configuration.
4. Use fresh absolute artifact paths under `out/pix-manual/` and create the
   parent directory. Keep captures out of commits. Server-relative paths depend
   on its capture directory and working directory.

For test optimization, rank completed individual leaf durations, excluding
overlapping group-node totals. Launch the chosen leaf in its own process with
an exact registered selector and verify the expected leaf count. For a long
multiphase leaf, inspect bounded early and later windows; retain each window's
offset and observed phase. An early startup trace cannot characterize later work.
Use phase dominance to choose the next change, adding temporary detail only
when needed to distinguish its cost; a cheap copy phase does not justify a copy
redesign merely because its implementation looks wasteful.

The pinned official release in `local-services/pix/version.txt` is the default
deployment. A source build is an explicit tooling experiment: verify both live
capabilities and result compatibility in the intended client before adopting it.
Building another checkout does not replace `local-services/pix/pix-mcp.exe`.
Compare path, hash, and timestamp; Cargo package versions can stay unchanged,
and the idempotent installer can retain an existing executable at the pin.
Record the MCP server release and executable identity separately from the
installed PIX application or `pixtool.exe` version. A PIX application version
reported by status does not identify the connected MCP server binary.

## Choose capture mode

- For CPU scopes, follow the normal launch-and-attach timing workflow below.
- For an initial GPU triage, use `pix_capture_and_analyze` or
  `pix_gpu_capture_launch` with the executable and destination paths. Start with
  one frame; use multiple frames only for an explicit workload need.
- PID GPU capture requires a process launched under PIX. Do not attach to a
  separately started game or treat `pix_launch`'s launcher PID as the game PID.
- `pix_programmatic_capture` needs the target to invoke PIX's in-process capture
  API at the selected region. Verify instrumentation exists before choosing it.
- Normal GPU capture does not require elevation. CPU/GPU timing capture requires
  an elevated server. Use only controls advertised by the live schema.

## Capture CPU scopes: normal launch and attach

1. Verify that the connected server reports ready and elevated. Follow the
   harness's permitted elevation/setup route if needed, then verify again.
   A GUI launched with `RunAs` or a shell's `require_escalated` setting does not
   establish the MCP server's token. A separately started elevated stdio server
   does not reconnect this chat; its PID alone is not evidence of connection.
2. Launch the instrumented workload normally with its required working directory,
   environment, arguments, and configuration. Obtain its actual running PID and
   attach `pix_timing_capture` to that PID. CPU timing does not need GPU launch
   injection. Routine timing work needs no custom elevation helper or pidfile
   handshake. Keep workload completion and exit status visible.
3. Choose a short representative interval. In the live schema, look for explicit
   CPU-sampling and callstack controls; preset names alone do not prove sampling
   is disabled. For sampling-free capture, explicitly disable CPU samples and
   optional callstack collection using documented controls, including presets or
   overrides only when exposed. Inspect reported effective settings.
   Enable GPU timings only when needed. PIX events are independent of sampling.
   The tool-call timeout must exceed capture duration plus attachment,
   finalization and response overhead. Verify any server-specific margin in the
   deployed version; do not assume a fixed 30s margin. In Codex inspect the
   effective `mcp_servers.<name>.tool_timeout_sec` (default 60s). A 60-65s capture
   cannot reliably finish through a 60s deadline; 120s is an example client
   allowance, not an engine/test timeout change. If configuration changes are
   outside the task, choose a meaningful shorter capture and report the limit.

Process discovery must parse complete structured output with explicit zero,
single and multiple-match handling. Never strip the leading array bracket by
slicing JSON from its first opening brace, or suppress parse errors. Retain PID
creation time and discovery failures; a missing parsed PID is not proof the
application was never running. Shell helpers must return while the workload is
still live; a buffered launch response is not a reliable attachment trigger.

Some installed versions expose only `process_id`, `duration_ms`, and
`output_path`, with no effective settings in the result. That schema cannot
enforce or verify sampling-free capture. Do not invent preset/override
arguments or call such a capture sampling-free. When UI use is permitted, use
PIX's Timing Capture options UI: disable CPU Samples and context-switch callstacks, select the
needed additional data, and record those settings with the bounded capture.
If UI use or required privileges are unavailable, retain the reproducible
request and report capture verification as unavailable.

The documented pixtool argument transport rejects many ordinary CLI arguments,
including prefixed tokens and values with spaces. Use the game's supported config
or autoexec mechanism after checking its actual lookup and precedence; do not
silently drop arguments. This restriction concerns PIX launch transport, not the
normal workload launch used for CPU timing. GPU PID capture still requires a
PIX-launched target.
Bound timing duration and programmatic waits through the discovered tool schema.
Do not assume an arbitrary launch/capture mode gracefully exits the target.

## Inspect CPU timing evidence

Native CPU timing queries are not exposed by this MCP integration;
`pix_get_event_list` exposes D3D12/GPU events, not CPU timing scopes. For file-only
or JSON analysis, read [timing-queries.md](references/timing-queries.md) and use the
[maintained marker helper](scripts/query_timing_capture.py). That conditional
route uses Microsoft's supported read-only SQLite interface and the trusted
installed `PixStorage.dll`, not private DLL interfaces. Select an explicit OS PID
and a unique captured process instance; retain the original `.wpix` as the source
of truth. Query selected scopes first rather than joining all events to timing
views. The helper's optional Chrome JSON contains named marker intervals only;
unreliable exclusive wall time is `null`, and sampled stacks are not reconstructed.

Keep Duration, Execution, and Stall distinct when separately querying supported
timing views. Preserve units, capture bounds, clipping, process/thread selection,
and missing-data limits. Table presence alone does not establish sample/callstack
coverage. Run analysis/export after timed workload comparisons so it does not
compete with the workload being measured.

Inspect meaningful nesting, phase boundaries, execution threads, and wait regions
through that evidence. A wait-call scope can include predicate checks, scheduling,
or native callbacks; its full duration is not necessarily blocked time. Respect
the user's file-only/no-PC-control preference. If required data is unavailable,
report the limitation; offer PIX UI inspection only when the user permits it.

## If the capture response fails

Check the requested artifact on the actual host and recheck server state before
retrying. Host and sandbox process/file visibility can differ: establish the
execution context before declaring a process stale or a file absent. Preserve
the error, capture path, and effective request. A transport/response error may
follow successful capture creation; a nonempty file does not prove analysis.
Distinguish capture creation, client response delivery, and successful inspection.
Avoid blind retries, repeated UAC prompts, or terminating unrelated processes.
Client timeout/cancellation is not proof that pixtool or the target terminated.
Any required process bound needs an enforceable watchdog and owned-process
cleanup, not merely a timeout written in the plan. Validate success by inspecting
the requested process/phase evidence. Nonempty files and `clipped=0` on paired
intervals do not establish that an earlier long phase was captured.

For suspected client compatibility, inspect the actual response shape and compare
against the pinned official release in an isolated location with a verified
release archive. Do not overwrite the active executable merely to compare it.
New content types such as `ResourceLink` can be a compatibility hypothesis;
success in a different raw-stdio client does not prove the chat client's cause
or establish a universal unsupported-content rule. Keep source-build experiments
recoverable and preserve the verified default when an experiment is not adopted.

## Analyze evidence

1. Inspect structured results and warnings, not only a success status.
   `pix_analyze_frame` provides heuristic triage. Request available counters
   through the installed schema when costs are needed; absent timing is not zero.
2. Use bounded `pix_get_event_list` pages for event structure and
   `pix_list_counters` for counter availability. Export large event lists to CSV.
3. Use `pix_get_screenshot` for the captured frame; inspect its result and open
   the capture with `pix_open_capture` when deeper UI inspection is needed and
   the user permits UI use.
4. `pix_run_analysis` validates debug-layer replay. Its output does not contain
   the complete debug-layer diagnostic stream; an empty issue list cannot prove
   the application emitted no diagnostics.

## Compare and report

`pix_compare_captures` can compare event structure and optional timing aggregates.
Check completeness, warnings, and whether it fell back to `metadata_only`.
Metadata, file size, and timestamps do not establish a performance change.
Use matching workloads and repeated paired measurements for performance claims;
report sample spread and capture overhead. Separate measured costs from heuristic
triage. Preserve capture paths, binary/configuration identity, effective settings,
and any unavailable counters or failed replay in the validation record.

For marker-coverage changes, also retain event rate and trace bytes per second
over comparable intervals. Compare matched baseline/candidate binaries to assess
added marker cost, and capture enabled/disabled on the same instrumented binary
to assess collection perturbation. Match optimization and logging settings:
`msvc-profile` and `msvc-rel` are not a marker-only comparison by default.
Do not sum inclusive nested or parallel durations into elapsed time. A successful
capture, event count, or smaller file alone does not establish an improvement.

If MCP tools are unavailable, report that the configured server is not callable
in this session. Use the supported local file-query route when available and
within the user's scope; otherwise retain the reproducible request and report
the missing evidence. Do not invent results.
