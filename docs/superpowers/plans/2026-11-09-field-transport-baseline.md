# World light field baseline: probe correctness and occupancy budget

Baseline gate for [Field Transport and Wave Occupancy](2026-11-09-field-transport-and-wave-occupancy.md).
This records the numbers later tasks are judged against: the validated probe scan,
per-pass GPU time at 1024², and the GPU occupancy counters. No engine source was
modified by this task.

## Build identity

| fact | value |
|---|---|
| branch / HEAD | `particles` / `6e7df3771e191d7146b952e64774955fc81d5a5e` |
| binary | `out/build/clang-rel/game/app.game`, 79,177,240 bytes |
| binary sha256 | `8a504a088d1901cccfef32d76bdb62ba14c8b2193cfe81447d94cbc7e91d0f56` |
| binary mtime | 2026-11-09 03:56:23 +0100 |
| newest app source | `App.Renderer.WorldLightSourceGpu.cpp` 03:55:11, `world_light_source.slang` 03:54:43 |
| build check | `cmake --build out/build/clang-rel --target app.game` compiled and linked nothing (glob re-check + asset staging only) |
| staged shaders | restaged 2026-11-09 04:24:14 |

The binary is current for HEAD: both producer-fix inputs (`4615ea9`) predate the
link, and `e6aacec`'s DWARF release flags are present (9 `debug_*` sections). The
`4615ea9`/`6f046b8` commits were authored after their working-tree edits, so
their commit timestamps do not indicate a stale binary. The only source newer
than the link is `lib/engine/tests/app/App.WorldLightField.Tests.cpp`, which is
not part of `app.game`.

Host: Linux (CachyOS), AMD Ryzen 9 3950X, NVIDIA RTX 2080 (TU104, 46 SMs, 32-lane
warps, 64 warps/SM, 65,536 32-bit registers/SM), driver 615.78.08. Nsight Graphics
2026.4.0.0 (build 39228586), Nsight Systems 2026.5.1.161.

## Probe correctness (validated)

The scan below is the staging-fix lane's validated result on this build (commit
`4615ea9` plus the air-probe test in `6f046b8`); the values are copied from that
report and were not re-derived here. It is a **six-site synthetic-world scan**
(the out-of-world site is sampled twice: deep outside and near the edge) **plus
one real-game sample**, each evaluated through `evaluate()` as composite / diffuse
/ specular in linear RGB:

| site | cell type | position (m) | composite | diffuse | specular |
|---|---|---|---|---|---|
| lamp | emissive solid | (40.25, 40.25) | 0.446/0.549/0.754 | 0.403/0.462/0.580 | 0.418/0.515/0.711 |
| air next to lamp | vacuum | (40.75, 40.25) | 0.529/0.544/0.572 | 0.346/0.407/0.528 | 0.525/0.540/0.568 |
| open room | vacuum, sky | (100.25, 100.25) | 0.250/0.500/1.000 | 0.241/0.481/0.963 | 0.250/0.500/1.000 |
| sealed cavity | vacuum in shell | (76.75, 51.75) | 0/0/0 | 0/0/0 | 0/0/0 |
| solid next to air | rock wall | (35.25, 40.25) | 0.050/0.050/0.050 | 0.056/0.105/0.205 | 0.054/0.054/0.054 |
| outside world | out of world | (-100, -100) | 0/0/0 | 0/0/0 | 0/0/0 |
| outside world (near) | out of world | (-4, -4) | 0.035/0.070/0.141 | 0.035/0.070/0.141 | 0.035/0.070/0.141 |

Real-game air cell adjacent to the emissive solid: cell `(362, 490)`, position
`(181.17, 244.83) m`, composite 2.500/1.476/0.445, diffuse 0.894/0.526/0.157,
specular 2.216/1.307/0.393. The same sample was reproduced live during the
baseline run (`game-probe.log`, position `(181.16667, 244.83334)`) and matches at
the printed precision.

What the scan establishes:

- the lamp's emissive solid is non-zero on every channel, and the adjacent vacuum
  cell's composite exceeds 0.5 on all channels;
- the open room is sky-dominated, with diffuse ≈ 0.963 × composite;
- the sealed cavity and the out-of-world sample are exactly zero;
- the solid sample next to air is non-zero and finite;
- the earlier `diffuse == 0` reading was a bad scan; `evaluate()` mode dispatch is
  correct.

**Per-level (0..5) output does not exist.** `evaluate()` returns one
composite/diffuse/specular triple per call over the whole six-level cascade, so the
plan's original requirement for per-level values is not satisfiable without an API
change. The six-level composite is pinned against the CPU oracle by
`composite_matches_cpu_reference` (`lib/engine/tests/app/App.WorldLightField.Tests.cpp`),
which compares the GPU `kComposite` result with `compositeReference` over a
synthetic field within 1e-4 per channel. That test, not a per-level readback, is
the numerical contract.

**Non-asserted observation.** The near-outside sample at `(-4, -4)`, inside the
bilinear footprint of the world edge, returns a non-zero `sky × 9/64` leak. It is
recorded for later tasks as an observation, not a gate: it is the current bilinear
edge contract, not a producer fault.

**Verdict: producer is correct on `4615ea9`+; no displaced peak and no flat
zero. The baseline may proceed.**

## Per-pass GPU time at 1024²

Capture: Nsight Graphics GPU Trace, 106 frames, `--map-size=1024 --auto-exit=31`,
`--start-after-ms 10000 --max-duration-ms 2000`, Turing "Throughput Metrics",
clocks locked to base. Durations are hardware `gpu__cycles_elapsed.sum` from
`BASE/GPUTRACE_REGIMES.xls`, converted at the 1,513.2 MHz clock calibrated against
the paired CPU-timed `D3DPERF_EVENTS.xls` ranges (the two agree within 0.3 % on
every dense marker).

| pass (marker) | frames | mean ms | median ms | p95 ms | min ms | max ms |
|---|---:|---:|---:|---:|---:|---:|
| `WorldLight.FieldUpdate` | 105 | **10.529** | 10.488 | 13.375 | 6.282 | 13.952 |
| └ `WorldLight.FieldTrace` | 105 | 10.249 | 10.225 | 13.117 | 6.022 | 13.692 |
| └ `WorldLight.FieldCopy` | 105 | 0.281 | 0.260 | 0.278 | 0.254 | 1.187 |
| `Grid.Terrain` | — | **not present** | — | — | — | — |
| `Grid.Translucent` | — | **not present** | — | — | — | — |
| `Particles.Draw` | 105 | 0.824 | 0.676 | 1.989 | 0.265 | 2.574 |
| `PBMPM.Tick` (ticking frames) | 51 | 2.464 | 2.306 | 4.043 | 1.826 | 4.143 |
| `PBMPM.Tick` (amortised, 106 frames) | 106 | 1.185 | — | — | — | — |
| `Particles.Prepare` | 105 | 0.140 | 0.114 | 0.132 | 0.104 | 1.868 |
| **frame time** (steady) | 100 | **19.069** | 18.994 | 22.322 | 15.003 | 23.706 |

Frame time over all 106 captured frames: mean 18.904, median 18.943, p95 22.296 ms
(the first frame is truncated to 3.843 ms).

Notes on the table:

- `Grid.Terrain` and `Grid.Translucent` have **no GPU marker in HEAD**. The
  strings exist nowhere in the current source or in this repository's history;
  the earlier analysis came from a build carrying extra uncommitted markers.
  They cannot be reported as named passes from this baseline. Task 7 restores the
  pass-level `Grid.Terrain` marker (and its translucent equivalent).
- The unmarked remainder is computed **per frame**, not by subtracting window
  means: `remainder[f] = frame_time[f] − Σ(top-level marker ms in frame f)`, over
  the 101 frames at indices 5..105 (the first five are dropped as warm-up, and
  every later frame that has both a frame-time row and hardware regimes rows is
  kept). That gives **mean 6.414, median 6.430, p95 7.471 ms/frame**. Subtracting
  the published pass means from the 100-frame steady frame-time mean instead gives
  `19.069 − (10.529 + 0.824 + 1.185 + 0.140) = 6.391 ms`; the 0.023 ms (0.4 %)
  difference is window mismatch — the field, particle and prepare rows exist on
  105 of 106 frames, `PBMPM.Tick` is amortised over all 106 frames but runs on
  only 51, and the frame-time mean uses indices 5..104. Use the per-frame figure
  for attribution and the mean difference only as an arithmetic check.
- The remainder contains the terrain and translucent grid draws, the UI pass
  (0.014 ms measured), post/present gaps and trace jitter. For scale, the same
  1024² command shape on the earlier instrumented build measured `Grid.Terrain`
  4.081 ms and `Grid.Translucent` 1.440 ms, which is consistent with it.
- `Particles.Prepare`'s `D3DPERF_EVENTS.xls` row is corrupt in this export (a
  leaked range reports a 510 ms mean); the hardware-timed 0.140 ms above is the
  usable value.
- `PBMPM.Tick` runs on 51 of 106 frames (30 Hz simulation against the render
  rate) at 2.464 ms per ticking frame; non-ticking frames hold a leaked constant
  4.094 ms in the CPU-timed export, which the hardware cycles contradict.
- `WorldLight.FieldTrace` is 97 % of `WorldLight.FieldUpdate`; the copy and the
  update bookkeeping are not the cost.

## Occupancy counters

Capture: Nsight Systems GPU metrics, `tu10x-gfxt`, 1 kHz, 8 s from t+10 s. Values
are device-wide samples attributed to PPR by GPU context residency (2,079
`RESTORE_START`/`SAVE_END` intervals, 7.659 s of the 8.023 s window), excluding
200 µs at each interval edge; 5,467 interior samples per metric, median sample
spacing 0.932 ms. The window contains 536 presents.

| metric | mean | p95 | max | median |
|---|---:|---:|---:|---:|
| GPU Active | 99.04 % | 100 | 100 | 100 |
| SM Issue Active | 21.65 % | 36 | 44 | 22 |
| PS Warp Can't Launch | 32.23 % | 100 | 100 | 2 |
| PS Warp Can't Launch - Register Limited | 28.02 % | 100 | 100 | 0 |
| CS Warp Can't Launch - Register Limited | 0.76 % | 8 | 13 | 0 |
| Unallocated Warps in Active SMs | 48.02 % | 60 | 77 | 51 |
| L1 Throughput | 17.14 % | 35 | 37 | 15 |
| L2 Hit Rate | 69.78 % | 97 | 98 | 71 |
| VRAM Throughput | 10.63 % | 30 | 56 | 7 |

Phase split, which is what carries the interpretation (samples gated by the
dominant warp class):

| metric | PS-dominant (Pixel Warps ≥ 20 %, n=1794) | CS-dominant (Compute Warps ≥ 20 %, n=2209) |
|---|---:|---:|
| Warp Can't Launch - Register Limited | **81.12 %** (median 94, p95 100) | **0.92 %** (median 0, p95 8) |
| Warp Can't Launch (all reasons) | 88.30 % (median 99) | — |
| Unallocated Warps in Active SMs | 52.96 % | 49.52 % |
| SM Issue Active | 29.41 % | 23.25 % |
| L1 Throughput | 26.04 % | 16.78 % |
| L2 Hit Rate | 92.48 % | 60.28 % |
| VRAM Throughput | 4.88 % | 15.28 % |

Reading:

- The GPU is **99 % active**: this is a GPU-bound frame, and the baseline is not
  measuring a partially idle device.
- **Frame-wide PS warp launch is register limited; terrain cannot yet be named as
  the cause.** Whenever the PS dominates, warp launch fails on register allocation
  81 % of the time (median 94 %), while L1 and VRAM are far from saturated. That is
  consistent with terrain shading being the dominant PS work — a full-screen
  heavy fragment shader against small particle, UI and background draws — but the
  counter is context-wide,
  `Grid.Terrain` has no marker in this build, and no per-pass attribution exists.
  The attribution becomes measurable only after Task 7 restores the pass-level
  marker. The ≤ 40 registers/thread terrain budget is the hypothesis this evidence
  supports, not a measurement.
- **Field compute is not register limited.** `CS Warp Can't Launch - Register
  Limited` is ~1 % with a p95 of 8 %, even though roughly half the warp slots in
  active SMs stay unallocated. The field's occupancy limit is its launch shape:
  `world_light_trace.slang` uses `[numthreads(64,1,1)]`, and 16 blocks/SM caps it
  at 32 warps/SM regardless of registers — the plan's ruling on 128-thread blocks
  is consistent with this measurement.
- `Unallocated Warps in Active SMs` (48 % mean) is the honest measure of idle warp
  slots in active SMs; it is not a per-shader occupancy figure.

Unprofiled frame pacing on the same build (Nsight Systems, present-to-present,
535 intervals): mean 14.94, median 14.88, p95 18.07 ms (≈ 67 fps). The GPU Trace
run reports a 19.07 ms device frame under tracing with clocks locked to base, so
the trace inflates absolute time; use the same command shape for every
comparison and do not mix traced and untraced numbers.

## Registers per thread: known gap

**Registers/thread and warps/SM per shader could not be obtained with the CLI on
this Turing GPU, so the plan's per-shader register budgets remain aspirational.**

- `SHI_REGISTERS` is not part of the GPU Trace CLI export; the exported set is
  `D3DPERF_EVENTS`, `FRAME`, `GPUTRACE_FRAME`, `GPUTRACE_REGIMES`, `REPRO_INFO`.
- The capture requests `Collect Shader Pipelines: Yes`, and the `.ngfx-gputrace`
  binary contains no register/launch-metric records.
- One attempt with `--real-time-shader-profiler --multi-pass-metrics` completed a
  capture but produced the identical six exports; `REPRO_INFO.xls` records
  `Real-Time Shader Profiler: Disabled` and `Multi-Pass Metrics: Disabled`. The
  Nsight Systems metric set `tu10x-gfxt` likewise exposes no per-shader register
  count.

Windows and Linux tooling behave the same way here; a GUI capture with the
Real-Time Shader Profiler is the documented offline path. Until one is taken,
registers/thread for the field compute and terrain pixel shaders are unknown, and
the `≤ 48` / `≤ 40` register targets cannot be judged directly — only through
the register-limited stall counters above.

## Budget verdict

| budget | target | measured | verdict |
|---|---|---|---|
| `WorldLight.FieldUpdate` at 1024² | ≤ 6 ms | **10.529 ms mean, 10.488 median, 13.375 p95** | **over by 4.53 ms at mean (75 %) and 7.38 ms at p95** |
| terrain PS registers | ≤ 40 regs (≥ 51 warps/SM) | not measurable; frame-wide PS register-limited stalls 81 % of PS-dominant time | unverified; consistent with, but not attributed to, an over-budget terrain PS |
| field CS registers | ≤ 48 regs with 128-thread blocks | not measurable; CS register-limited ~1 % | unverified, registers are not the observed limit |
| frame time | no regression | 19.07 ms traced / 14.9 ms untraced at 1024² | baseline recorded |

`WorldLight.FieldUpdate` is the only pass over its stated budget. It is 97 %
`FieldTrace`, so the gap is in the cascade trace, not in the copy or the source
upload. The terrain budgets cannot be separated from the unmarked 6.4 ms
remainder until Task 7 restores the grid markers.

## Evidence and reproduction

Raw captures and analysis scripts (not committed; ~780 MB):

| artifact | path |
|---|---|
| GPU Trace export + `.ngfx-gputrace` | `/tmp/opencode/baseline-20261109-043501/ngfx/` |
| RTSP attempt (no register data) | `/tmp/opencode/baseline-20261109-043501/ngfx-rts/` |
| Nsight Systems `.nsys-rep` + sqlite | `/tmp/opencode/baseline-20261109-043501/nsys/` |
| game log with the live probe sample | `/tmp/opencode/baseline-20261109-043501/game-probe.log` |
| per-marker, regime, occupancy, final table scripts | `analyze_ngfx.py`, `analyze_regimes.py`, `analyze_nsys.py`, `final_ngfx.py` |

Commands:

```sh
ngfx --activity "GPU Trace Profiler" --architecture Turing \
  --metric-set-name "Throughput Metrics" \
  --exe out/build/clang-rel/game/app.game --dir /home/bek/Projects/ppr \
  --args "--map-size=1024 --auto-exit=31" \
  --start-after-ms 10000 --max-duration-ms 2000 \
  --auto-export --output-dir /tmp/opencode/baseline-20261109-043501/ngfx

nsys profile --trace=vulkan,vulkan-annotations --vulkan-gpu-workload=individual \
  --sample=none --cpuctxsw=none --gpu-metrics-devices=0 \
  --gpu-metrics-set=tu10x-gfxt --gpu-metrics-frequency=1000 --gpuctxsw=true \
  --delay=10 --duration=8 --kill=sigterm --wait=primary --force-overwrite=true \
  --output=/tmp/opencode/baseline-20261109-043501/nsys/ppr-baseline \
  out/build/clang-rel/game/app.game --map-size=1024 --auto-exit=30
```

## Gaps carried into later tasks

1. **No grid markers.** `Grid.Terrain`/`Grid.Translucent` must be restored before
   the terrain register/occupancy budget can be judged; Task 7 owns that marker
   work. This task was not allowed to edit source, and no terrain number is
   fabricated in its absence.
2. **No register counts.** A GUI GPU Trace with the Real-Time Shader Profiler is
   required to fill the `≤ 40` / `≤ 48` register targets.
3. **Trace overhead.** Every later measurement must use the command shape above,
   with clocks locked, and compare like with like.
4. **PBMPM cadence.** `PBMPM.Tick` is a 30 Hz pass; compare it per ticking frame
   or amortised, never against the leaked CPU-timed rows.
