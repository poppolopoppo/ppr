# Static world generation acceptance evidence

Implementation authority: [implementation prompt](static-world-generation-implementation-prompt.md).
Incremental changes and older evidence: [progress log](static-world-generation-progress.md).
Authoring commands and schemas: [content reference](../worldgen-content.md).

The seven-stage implementation is integrated through the runtime acceptance
sequence. Overall acceptance remains open: observed long update/presentation
gaps and the unverified gates below prevent a completion claim.

## Recorded environment and commands

Windows 11, Ryzen 9 3950X, 64 GiB RAM, RTX 2080; MSVC 14.51/VS 18 Insiders,
Release (`msvc-rel`), D3D12. Latest successful scripted runtime: `5caec5d`
plus the reviewed settling-window correction, before the native UI fixes.
The two profile cases ran sequentially without another build/test workload:

```text
app.game --worldgen-acceptance --worldgen-profile=frontier --worldgen-seed=42 --exit-after=300 --display.pacing=paced --display.cap_hz=60 --display.paced_vsync=false --display.throttle_unfocused=false
app.game --worldgen-acceptance --worldgen-profile=hollows --worldgen-seed=42 --exit-after=300 --display.pacing=paced --display.cap_hz=60 --display.paced_vsync=false --display.throttle_unfocused=false
```

Logs: `out/worldgen-scenario-v4-{frontier,hollows}.stdout.log`.
Exact commands, process exits and memory samples:
`out/worldgen-scenario-memory-v4.json`. The process wrapper pins the process
handle before sampling, so both recorded zero exits are actual exit codes.

## Runtime resource and publication checks

Both cases exited zero with the explicit acceptance PASS marker. This verifies
the scripted production requests and successful presentation witnesses; it does
not substitute for native-input interaction or establish frame responsiveness.

| Observation | Frontier | Hollows |
|---|---:|---:|
| Whole sequence elapsed, seconds | 64.752 | 64.803 |
| Stationary encoded draws / dispatches | 83 / 166 | 82 / 164 |
| Pan/zoom encoded draws / dispatches | 82 / 164 | 83 / 166 |
| Presented two-cell seam edit, ms | 156.979 | 142.361 |
| Seam atlas bytes / publications | 65,536 / 1 | 65,536 / 1 |
| Biome preview, seconds | 15.401 | 15.327 |
| Textured preview, seconds | 7.961 | 7.919 |
| New Game seed 1, seconds | 10.763 | 10.692 |
| New Game seed 42, seconds | 9.104 | 9.323 |
| Resource settling after replacement, seconds | 3.111 | 3.122 |
| Settled retained atlases / texture entries / texture slots | 1 / 7 / 7 | 1 / 7 / 7 |
| Observed snapshot peak atlases / texture entries | 2 / 14 | 2 / 14 |
| Whole-process peak working set, bytes | 1,049,784,320 | 1,045,434,368 |
| 100 ms sampled peak private memory, bytes | 1,267,372,032 | 1,266,905,088 |

Each stationary/pan window lasted at least two seconds and had zero atlas bytes,
writes, serializations, deferrals, growth waits, resident-table builds and uploads.
The seam edit had one resident-table build/upload and no serialization or growth
wait. Each preview/New Game uploaded 33,554,432 atlas bytes with one atlas
publication, one table build, 127 deferred preparation observations, and 259–261
table uploads while preparation progressed. Final settling could include one
remaining ring-table upload. The sequence returned to the initial texture-slot
baseline after repeated replacements.

Draw/dispatch counts describe encoded GPU-driven terrain commands, not completed
GPU executions or measured GPU time. Resource snapshots can miss peaks between
frames. Process memory includes active/candidate sessions and their other engine
resources; it is neither precise VRAM usage nor an allocation census.

## Responsiveness remains an open acceptance failure

Configured cadence was 60 Hz (16.667 ms). The latest scripted runs observed
maximum live update gaps of 32.998 ms (Frontier) and 47.859 ms (Hollows), and
retiring presentation-ack gaps of 63.739 and 58.873 ms respectively. Frontier's
maximum control-poll gap was 31.300 ms; its pacing/clock observation reached
32.609 ms. These independent
maxima and endpoint phase labels do not prove a common cause.

Frontier startup took 1,837.967 ms to the first successful loading presentation
and another 9,256.097 ms to the first world presentation. Loading/control work
remained observable, but asynchronous dispatch alone does not satisfy the cadence
requirement. No claim of acceptable responsiveness is made from these runs.
The preceding v3 Frontier run had a 157.633 ms initial layout update gap and
failed an eager two-second resource assertion. Bounded retirement now settles
before success under the existing overall timeout; the initial layout variance
is unresolved and cannot be attributed to that correction.

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
- Native UI evidence is retained under `out/worldgen-native-v1`: catalog-backed
  Basalt/Iron readouts, New Game confirmation, terrain-composition and diagnostic
  preparation modals, and a successful session reset. Diagnostic Cancel retained
  the world. Its action/capture took 495 ms including automation transport; this
  is not a precise engine latency measurement. This run exposed focus/follow,
  cancellation-notice and close-drain defects. Reviewed fixes are built, but
  native traversal, extension-name and clean-close reruns remain open. The first
  run exited 22 on close (`out/worldgen-native-v1-result.json`), not success.
- Saved GPU profiler capture, GPU terrain time, precise GPU-memory peak, worker
  utilization and true scheduler queue depth remain unmeasured. Existing scheduler
  outstanding-work counts combine queued and executing work.
- All four terrain/visibility entry points compiled for DXIL and SPIR-V
  (`out/worldgen-shader-targets-v1/report.json`, eight zero exits). Runtime
  Vulkan readbacks now passed the five selected terrain/visibility/catalog/
  diagnostic/transaction gates in three shuffled loops, with no `VUID-` errors
  (`out/worldgen-vulkan-gates-v2.log`). The first Vulkan run failed: a redundant
  always-zero vertex scalar generated a second push-constant range, readback
  targets lacked CopySource usage, and two tests used a stale observed completion
  fence after a host wait. These are corrected in PPR code/tests; third-party
  source is unchanged. Dev, Live and Release affected builds passed. D3D12
  three-loop regression is in progress; current full matrix remains required.
  Current IDE inspection is unavailable because
  the CLion router transport failed during discovery.

Third-party cleanup remains deferred until plan implementation is finished, per
the user. No new fork or private library patch is authorized.
