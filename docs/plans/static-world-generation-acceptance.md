# Static world generation acceptance evidence

Implementation authority: [implementation prompt](static-world-generation-implementation-prompt.md).
Incremental changes and older evidence: [progress log](static-world-generation-progress.md).
Authoring commands and schemas: [content reference](../worldgen-content.md).

The seven-stage implementation is integrated through the runtime acceptance
sequence. Overall acceptance remains open: observed long update/presentation
gaps and the unverified gates below prevent a completion claim.

## Recorded environment and commands

Windows 11, Ryzen 9 3950X, 64 GiB RAM, RTX 2080; MSVC 14.51/VS 18 Insiders,
Release (`msvc-rel`), D3D12. Current runtime implementation revision: `427a2fc`.
The two profile cases ran sequentially without another build/test workload:

```text
app.game --worldgen-acceptance --worldgen-profile=frontier --worldgen-seed=42 --exit-after=300 --display.pacing=paced --display.cap_hz=60 --display.paced_vsync=false --display.throttle_unfocused=false
app.game --worldgen-acceptance --worldgen-profile=hollows --worldgen-seed=42 --exit-after=300 --display.pacing=paced --display.cap_hz=60 --display.paced_vsync=false --display.throttle_unfocused=false
```

Logs: `out/worldgen-scenario-v2-{frontier,hollows}.stdout.log`.
Exact commands, process exits and memory samples:
`out/worldgen-scenario-memory-v2.json`.

## Runtime resource and publication checks

Both cases exited zero with the explicit acceptance PASS marker. This verifies
the scripted production requests and successful presentation witnesses; it does
not substitute for native-input interaction or establish frame responsiveness.

| Observation | Frontier | Hollows |
|---|---:|---:|
| Whole sequence elapsed, seconds | 61.360 | 62.813 |
| Stationary encoded draws / dispatches | 83 / 166 | 81 / 162 |
| Pan/zoom encoded draws / dispatches | 82 / 164 | 81 / 162 |
| Presented two-cell seam edit, ms | 141.580 | 156.418 |
| Seam atlas bytes / publications | 65,536 / 1 | 65,536 / 1 |
| Biome preview, seconds | 16.439 | 16.775 |
| Textured preview, seconds | 6.394 | 6.450 |
| New Game seed 1, seconds | 8.994 | 9.483 |
| New Game seed 42, seconds | 9.190 | 9.484 |
| Settled retained atlases / texture entries / texture slots | 1 / 7 / 7 | 1 / 7 / 7 |
| Observed snapshot peak atlases / texture entries | 2 / 14 | 2 / 14 |
| Whole-process peak working set, bytes | 1,010,188,288 | 1,014,214,656 |
| 100 ms sampled peak private memory, bytes | 1,225,273,344 | 1,232,449,536 |

Each stationary/pan window lasted at least two seconds and had zero atlas bytes,
writes, serializations, deferrals, growth waits, resident-table builds and uploads.
The seam edit had one resident-table build/upload and no serialization or growth
wait. Each preview/New Game uploaded 33,554,432 atlas bytes with one atlas
publication, one table build, 127 deferred preparation observations, and 259–260
table uploads while preparation progressed. Final settling could include one
remaining ring-table upload. The sequence returned to the initial texture-slot
baseline after repeated replacements.

Draw/dispatch counts describe encoded GPU-driven terrain commands, not completed
GPU executions or measured GPU time. Resource snapshots can miss peaks between
frames. Process memory includes active/candidate sessions and their other engine
resources; it is neither precise VRAM usage nor an allocation census.

## Responsiveness remains an open acceptance failure

Configured cadence was 60 Hz (16.667 ms). Frontier observed a 205.286 ms maximum
live update gap and 210.627 ms retiring presentation-ack gap; Hollows observed
77.219 and 64.457 ms respectively. Frontier's maximum control-poll gap was
32.200 ms; its pacing/clock observation reached 32.373 ms. These independent
maxima and endpoint phase labels do not prove a common cause.

Frontier startup took 1,843.882 ms to the first successful loading presentation
and another 9,156.736 ms to the first world presentation. Loading/control work
remained observable, but asynchronous dispatch alone does not satisfy the cadence
requirement. No claim of acceptable responsiveness is made from these runs.

## CPU corpus, images and accounting

The bounded corpus covers each shipped profile at seeds 0, 1, 42 and maximum
u64. CPU reports and four overviews per case are under
`out/worldgen-acceptance/<profile>-<seed>`; actual GPU closeups are retained in
the output tree as recorded in the progress log. Thirty-two overview images and
thirty-two GPU closeups decoded and were inspected. The data-only material
extension has CPU/GPU evidence under `out/worldgen-extension-acceptance`.

Fresh seed-42 CPU exports under `out/worldgen-memory-acceptance/<profile>-42`
decode as 1024×1024 RGB and are byte-identical to the inspected corpus images.
Verification: `out/worldgen-memory-image-verification-v1.json`.

| CPU observation, bytes | Frontier | Hollows |
|---|---:|---:|
| Retained known generation payload | 134,451,392 | 134,572,880 |
| Largest known phase scratch | 2,101,584 | 2,101,584 |
| Raster scratch bound | 2,020,712 | 2,037,092 |
| Whole-process peak working set | 159,088,640 | 159,354,880 |
| 100 ms sampled peak private memory | 183,623,680 | 183,898,112 |

Known generation accounting excludes opaque allocations, allocator overhead,
reallocation overlap, stacks and content/physics/GPU storage. It is separate from
process-memory observations and does not establish total peak memory.

## Remaining validation

- The twelve-entry dev/ASan CTest matrix passed at `427a2fc` in 905.78 seconds
  (`out/worldgen-final-matrix-v2.log`). Repeated app/render-lane and asset/render gates passed three loops each.
  The fresh MSVC Live build passed all six executable targets in 221.77 seconds.
  Current dev and Release builds and the earlier three-loop affected suites
  passed; see the progress log for exact revisions and logs.
- A reviewed follow-up bounds retired render-world payload disposal. Its new
  GPU regressions passed three shuffled loops; broader repeated gates and runtime
  measurements continue. The preceding
  matrix tested the prior binaries.
- Actual single-dupe traversal, Inspector/material-extension interaction,
  native input/Cancel latency and progress-modal phase captures remain open.
  The installed Windows Computer Use helper now lists native windows; interaction
  will run after the GPU test queue is idle.
- Saved GPU profiler capture, GPU terrain time, precise GPU-memory peak, worker
  utilization and true scheduler queue depth remain unmeasured. Existing scheduler
  outstanding-work counts combine queued and executing work.
- All four terrain/visibility entry points compiled for DXIL and SPIR-V
  (`out/worldgen-shader-targets-v1/report.json`, eight zero exits). Runtime
  D3D12 evidence does not establish Vulkan execution or portability.
  Current IDE inspection is unavailable because
  the CLion router transport failed during discovery.

Third-party cleanup remains deferred until plan implementation is finished, per
the user. No new fork or private library patch is authorized.
