---
name: pix-profiling
description: Capture and analyze PPR CPU timing scopes and app.game DirectX 12 work through the configured PozLabs pix-mcp server, including sampling-free timing, GPU triage, and bounded comparisons.
---

# PIX Profiling

Use the `pix-mcp` server configured in `opencode.json` under `mcp.servers`.
The upstream [README](https://github.com/PozLabs/pix-mcp/blob/main/README.md)
owns server capabilities and compatibility notes. Discover the installed tool
schemas before calling them: a configured server may lag upstream. Do not
reinstall the server or substitute an unrelated PIX integration.

Coverage policy lives in [AGENTS.md](../../../AGENTS.md#runtime-cost); the
[profiling guide](../../../docs/profiling.md) explains HAL scope placement and
interpretation. HAL helpers emit CPU events from inside the target process.
An external capture tool cannot insert those scopes or turn them into GPU timings.

## Prepare a reproducible workload

1. Use `build-system` to build `app.game` with `msvc-profile` when profiling
   markers are needed. Verify the binary and workload configuration correspond
   to the change under investigation.
2. Call `pix_status`; inspect available readiness fields and privileges, then
   discover capture/analysis schemas. Do not assume that status exposes commands,
   effective settings, or Developer Mode readiness. Windows Developer Mode is required for analysis
   operations. Normal GPU capture does not require elevation.
3. Choose a repeatable scene, camera, edit workload, warmup, and capture point.
   Record effective display/focus settings. Game arguments/configuration are
   resolved by `game/Game.Config.*`, not a hand-written loop in `main.cpp`.
   Profiling defaults can disable unfocused throttling, but explicit settings
   can override them; verify the effective configuration.
4. Use fresh absolute artifact paths under `out/pix-manual/` and create the
   parent directory. Keep captures out of commits. Server-relative paths depend
   on its capture directory and working directory.

## Choose capture mode

- For an initial GPU triage, use `pix_capture_and_analyze` or
  `pix_gpu_capture_launch` with the executable and destination paths. Start with
  one frame; use multiple frames only for an explicit workload need.
- PID GPU capture requires a process launched under PIX. Do not attach to a
  separately started game or treat `pix_launch`'s launcher PID as the game PID.
- `pix_programmatic_capture` needs the target to invoke PIX's in-process capture
  API at the selected region. Verify instrumentation exists before choosing it.
- For CPU/GPU timelines, use `pix_timing_capture` with only controls advertised
  by its installed schema. Inspect effective settings when the response exposes
  them. It requires an elevated server;
  follow the harness approval policy for elevation. Timing-capture analysis
  currently belongs in the PIX UI, rather than GPU-analysis tools.

## Capture CPU scopes without sampling

1. Choose a short representative interval and an instrumented binary. In the
   timing-capture schema, look for explicit CPU-sampling and callstack controls;
   preset names alone do not prove that CPU sampling is disabled. Disable CPU
   samples and optional callstack collection using documented supported controls,
   and verify the reported effective settings. Enable GPU timings only when the
   investigation needs them. PIX events are independent of CPU sampling.
2. Some installed versions expose only `process_id`, `duration_ms`, and
   `output_path`, with no effective settings in the result. That schema cannot
   enforce or verify sampling-free capture. Do not invent preset/override
   arguments or call such a capture sampling-free. Use PIX's Timing Capture
   options UI: disable CPU Samples and context-switch callstacks, select the
   needed additional data, and record those settings with the bounded capture.
   If UI access or required privileges are unavailable, retain the reproducible
   request and report capture verification as unavailable.
3. Inspect CPU scopes in the PIX timing UI: meaningful nesting, phase boundaries,
   expected execution threads, and explicit wait regions. A wait-call scope can
   include predicate checks, scheduling, or native callbacks. Do not equate its
   full duration with pure blocked time or infer GPU cost from a CPU render scope.
   `pix_get_event_list` exposes D3D12/GPU capture events; it does not substitute
   for CPU timing inspection.

The repository pins the local server in `local-services/pix/version.txt`. Verify
the connected server's schema rather than assuming the pin, upstream README, and
live capabilities agree. Reconcile or update that integration as a separate
bounded tooling change if automated sampling controls are required; do not
silently upgrade a binary during a profiling investigation.

The documented pixtool argument transport rejects many ordinary CLI arguments,
including prefixed tokens and values with spaces. Use the game's supported config
or autoexec mechanism after checking its actual lookup and precedence; do not
silently drop arguments or assume a separately launched process becomes capturable.
Bound timing duration and programmatic waits through the discovered tool schema.
Do not assume an arbitrary launch/capture mode gracefully exits the target.

## Analyze evidence

1. Inspect structured results and warnings, not only a success status.
   `pix_analyze_frame` provides heuristic triage. Request available counters
   through the installed schema when costs are needed; absent timing is not zero.
2. Use bounded `pix_get_event_list` pages for event structure and
   `pix_list_counters` for counter availability. Export large event lists to CSV.
3. Use `pix_get_screenshot` for the captured frame; inspect its result and open
   the capture with `pix_open_capture` when deeper UI inspection is required.
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

If tools are unavailable, report that the configured server is not callable in
this session and retain the reproducible capture request. Do not invent results.
