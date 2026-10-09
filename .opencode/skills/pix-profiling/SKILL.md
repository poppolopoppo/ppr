---
name: pix-profiling
description: Capture and analyze app.game DirectX 12 work through the configured PozLabs pix-mcp server, including GPU triage, timing capture, and bounded comparisons.
---

# PIX Profiling

Use the `pix-mcp` server configured in `opencode.json` under `mcp.servers`.
The upstream [README](https://github.com/PozLabs/pix-mcp/blob/main/README.md)
owns server capabilities and compatibility notes. Discover the installed tool
schemas before calling them: a configured server may lag upstream. Do not
reinstall the server or substitute an unrelated PIX integration.

## Prepare a reproducible workload

1. Use `build-system` to build `app.game` with `msvc-profile` when profiling
   markers are needed. Verify the binary and workload configuration correspond
   to the change under investigation.
2. Call `pix_status`; inspect installed PIX, advertised commands, privileges,
   and analysis readiness. Windows Developer Mode is required for analysis
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
- For CPU/GPU timelines, use `pix_timing_capture` with a supported preset and
  inspect the reported effective settings. It requires an elevated server;
  follow the harness approval policy for elevation. Timing-capture analysis
  currently belongs in the PIX UI, rather than GPU-analysis tools.

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

If tools are unavailable, report that the configured server is not callable in
this session and retain the reproducible capture request. Do not invent results.
