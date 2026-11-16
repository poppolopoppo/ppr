# PB-MPM baseline and multi-layer results

**Plan:** [PB-MPM baseline correction and multi-layer plan](../../.opencode/plan/2026-11-15-pbmpm-baseline-and-multilayer.md)
**Status:** Phase A in progress. A0, A2 (constitutive and precision), A3 and A4 complete; A1 stage 1 complete with stage 2 unbuilt; A5 blocked pending a plan-owner decision; A6 pending.
**Identity:** branch `pbmpm/phase-a` in worktree `/home/bek/.local/share/opencode/worktree/4bb70a/pbmpm-phase-a`, base `61df61b954dc804fdf571e10ee42222e6be7fbbd`. Solver, matter, shader and game-factory sources are identical to the planning pin `07b9801ca3fc0b8792978eea42dbbd631560d9d7`; later commits in this checkout are renderer/lighting work.

## Host and configuration

| Item | Value |
|---|---|
| Host | Linux, NVIDIA GeForce RTX 2080, driver 620.70.06 |
| Preset | `clang-dev` (Debug, ASan+UBSan, validation layers, `PPR_TEST_LOOPS=1`) |
| Toolchain | CMake 4.5.0, Ninja 1.13.2, Clang 23.1.5, Python 3.14.9, libc++ via `x64-linux-libcxx` |
| Backend | Vulkan (GPU-assisted validation + core check enabled) |

All timings below are this configuration. They are not Release figures and must not be quoted as runtime or product budgets.

## A0 — simulation suite census

Command (single-threaded, JUnit at `/tmp/opencode/a0-junit.xml`):

```sh
ctest --test-dir out/build/clang-dev -j1 --output-on-failure --output-junit a0-junit.xml \
  -R '^engine[.]tests[.]sim$|^engine[.]tests[.]app[.]matter|^game[.]tests$|^game[.]tests[.](matter|particle_tools|service_door|publication_hold)'
```

Result: 23 entries, 22 passed, 1 failed; total 1566.63 s.

| Entry | Time (s) |
|---|---:|
| `game.tests.matter_pressure_gpu` | 796.06 |
| `game.tests.matter_pressure_onset_gpu` | 240.51 |
| `game.tests` | 192.57 |
| `engine.tests.sim` | 52.86 |
| `engine.tests.app.matter_material_gpu` | 47.75 |
| `game.tests.matter_edit_gpu` | 24.51 |
| `game.tests.matter_publication_gpu` | 23.76 |
| `game.tests.matter_stall_gpu` | 16.91 |
| `game.tests.matter_restore_gpu` | 16.67 |
| `game.tests.matter_sand_gpu` | 14.46 |
| `game.tests.service_door_gpu` | 7.84 |
| `game.tests.particle_tools_gpu` | 5.06 |
| `game.tests.matter_coverage_gpu`, `game.tests.publication_hold_cpu` | 0.10–0.11 |
| App matter shards (10 entries, aggregated) | 175.20 |

## A0 — defects found and resolved

### A0-INFRA-1: fresh configure generated `import std` from libstdc++

A fresh `cmake --preset clang-dev` produced the standard module from `/usr/include/c++/16/bits/std.cc` while the build compiles with `-stdlib=libc++`; every dependent scan failed with `fatal error: 'bits/stdc++.h' file not found`. GCC 16 now ships `/usr/lib64/libstdc++.modules.json`, so CMake's compiler-support detection recorded it before `cmake/compiler/Clang.cmake` selected the libc++ manifest. The main checkout only still built because its generated files predate the host change.

Fix: `cmake/LibcxxModulesManifest.cmake` owns manifest discovery and is included from the root `CMakeLists.txt` before `project()`, so CMake's own probe never records the libstdc++ manifest for this build. `cmake/compiler/Clang.cmake` re-includes it; the second pass is a no-op and the selection message prints once.

Verified: `cmake --fresh --preset clang-dev` exits 0, prints the manifest once, and every generated `CXXDependInfo.json` resolves `/usr/share/libc++/v1/std*.cppm`.

### A0-TEST-1: liquid pool render leaf asserted a retired contract

`engine.tests.app.matter_material_gpu` → `app/matter_gpu/evolved_liquid_pool_has_no_interior_render_holes` failed at `precedence_ok == precedence_samples` with `0/156`.

Root cause: the renderer now gates terrain-over-particle occlusion on the committed particle-ownership grid (`grid_tiles.slang:2277-2287`, `terrainOccludesParticle` at line 2023), published by `WorldLightingPass::bindTerrainOwnership`. The fixture supplied only resolved cells and fine CH, so the ownership bit stayed clear and the density field painted over the injected terrain cell. Production publishes resolved cells and winners from one authority; the assertion encoded the pre-`62e06cf5` categorical-base contract.

Fix, in two increments:

1. `GridPass::setParticleOwnershipForTest` publishes a fixture winner grid **only together with** resolved cells, preserving the single-authority rule. The leaf publishes its own winner model; precedence became `156/156` for both sampled ticks.
2. That exposed a never-before-reachable late state (`outside_painted=1164`). The old assertion predates the accepted Gaussian reconstruction (sigma 0.15 native cell, truncated at ±3σ) and demanded zero paint in every non-nominated cell. It now classifies paint by Chebyshev distance from nominated cells: bands 1–2 are legal bleed and are reported; bands 3+ and the far-field probe must stay zero. `holes`, `precedence`, `samples` and `water_pixels` remain strict.

Bound derivation (in the test): particle reference radius `sqrt(V/pi) = 0.5642` cell, support `kParticleSurfaceRadiusScale * r = 0.8609` cell, plus `3 * kParticleSurfaceSmoothingSigma = 0.45` cell gives 1.3109 cell ⇒ band 2 is the last legal band.

Verified: `engine.tests.app.matter_material_gpu` 13/13 leaves pass.

## A0 — mandatory gates

| Gate | Label | Entries | Measured wall time | Command |
|---|---|---:|---:|---|
| Repeated-use simulation | `matter_fast` | 20 | 263.6 s (`-j4`) | `ctest --test-dir out/build/clang-dev -L matter_fast -j4 --output-on-failure` |
| Long-horizon stability | `matter_long` | 2 | 961.2 s (`-j2`) | `ctest --test-dir out/build/clang-dev -L matter_long -j2 --output-on-failure` |
| All simulation | both | 22 | 1225 s (sequential sum) | `ctest --test-dir out/build/clang-dev -L 'matter_(fast\|long)' -j4 --output-on-failure` |

Both gates are green on the A0 source state. `matter_fast` covers the sim suite, every app matter shard, and the game matter/edit/restore/stall/publication/sand/coverage/service-door/particle-tool/checkpoint-CPU entries. `matter_long` holds only the two long-horizon liquid-stability entries; it stays mandatory at phase end and delivery.

`game.tests` (192 s, broad game CPU incl. worldgen) is outside both, as are asset/rendering shards. Their matter coverage is duplicated by the gates above; add them when a change touches their containers.

## A0 — deletion audit

No test was removed. The census found no entry whose contract is proven elsewhere in the scope, and the two long-horizon entries cover liquid stability the multi-layer work depends on. Removal candidates must name a retained replacement for the contract they prove, so none qualify yet; the gate split absorbs the cost instead.

## Open items

- A zero-particle tick dispatches zero-size compute groups (validation error) and completes without a particle snapshot; this pre-existing issue is independent of K and should be rejected at admission or handled explicitly.
- A1 stage 2 (Elastic/Sand/Visco, K5/K10, N-substep snapshots, committed generator) is not started. A2 precision is characterized but its supported-motion-range question remains open; A5 is blocked on a plan-owner decision. See the sections below.
- D3D12/Windows verification is unavailable on this host and remains an unrun gate.
- Only the Debug/ASan/validation configuration has been measured; Release timing and cost evidence is pending.

## Mandatory gate budgets

| Gate / entry | Entries | Measured | Ceiling |
|---|---|---:|---:|
| `matter_fast` | 24 | **293.61 s** (`-j4`, K=5 default, resident race fixed) | **≤ 420 s exclusive** |
| `game.tests.matter_pressure_gpu` | 1 | 2286.84 s | **≤ 2750 s** |
| `game.tests.matter_pressure_onset_gpu` | 1 | 304.84 s | **≤ 370 s** |
| `matter_long` (both entries) | 2 | ≈ 2592 s (≈ 43 min), exclusive, 87 % average GPU utilisation | **≤ 3120 s** (sum of entry ceilings) |

Ceilings are task-local repeated-use budgets for this reference host and the Debug/validation configuration; they are not product budgets. They were recorded after the A0/A3 implementation rather than set prospectively against it.

The long entries were re-measured at the K=5 default on an exclusive host at 87 % average GPU utilisation: `matter_pressure_gpu` 2286.84 s and `matter_pressure_onset_gpu` 304.84 s, so the gate costs ~2592 s (about 43 min) and passes with the ceilings raised to 2750 s and 370 s, roughly 20 % margin on each. That cost is precisely why `matter_long` is a phase-end and delivery gate and not a repeated-use gate: it runs when a change reaches acceptance, not per iteration. The pre-K5 900 s and 300 s ceilings are superseded — under K=5 the 900 s ceiling truncated `matter_pressure_gpu` at a 900.26 s timeout (`/tmp/opencode/k5-long.log`) while the entry needs 2286.84 s. The 2.5–2.9× growth (2.87× on `matter_pressure_gpu`, 2.70× on the gate total) is the K=5 schedule — 152 dispatches per tick instead of 104, and 1.46× wall per tick at the A4 4096-row operating point — not a regression; the horizons these entries cover are unchanged.

`matter_fast` now holds 24 entries, and its 420 s ceiling was set before K=5, so it must be re-derived from a K=5 measurement instead of carried forward. No passing 24-entry K=5 reading exists yet: `/tmp/opencode/k5-fast2.log` stopped after 2 of 24 entries with no summary, so the re-run is pending. The only completed 24-entry K=5 reading so far took 314.75 s and failed the `app/matter_runtime_gpu/resident_completion_keeps_metadata_and_freezes_exact_demand_snapshot` leaf — the test-ordering bug resolved above — so it is cost evidence at K=5, not a gate certificate. The fast gate is therefore not certified at this source state; the long gate is green at K=5 under those ceilings.


## A1 — upstream reference harness (stage 1 complete)

The retained demo snapshot was incomplete (no `gpu.js`, `shader.js`, `buffer_factory.js`, `v.js`, and no bukkit kernels), so the harness drives the full upstream tree at `61749b6c2d2631ed243f86882752ba43196f2de7` (cloned to `/tmp/opencode/pbmpm-upstream2`).

Runtime: `webgpu@0.6.2` (Dawn bindings) with adapter `nvidia-geforce-rtx-2080`. This host has no alternative: Chromium exposes no `navigator.gpu`, and Node 22 has no built-in WebGPU.

Harness: `/tmp/opencode/pbmpm-harness` — upstream `sim/shader/buffer_factory/v/time.js` copied verbatim, a compute-only `gpu.js` replacement, a `fetch` shim serving `shaders/*.wgsl` through the upstream `//!include` / `//!insert` preprocessor, and direct particle seeding in the `particle.inc.wgsl` layout.

Stage 1 evidence (`out/liquid-k2-s1.json`):

| Check | Result |
|---|---|
| Determinism | Two separate processes byte-identical (sha256 `6a396a31…9509cbe`) |
| Gravity application | `displacement.y = -0.005555556` = `-32 · 2.5 · (1/120)²`, exactly once |
| Real P2G work | Bukkit args `[4,1,1,0]`, grid mass `64 · 1e7` present |
| Liquid state | `max |det F − 1| = 0`, `liquidDensity ∈ [1,1]`, `nonFiniteCount = 0` |
| Particles | 64 seeded, 3 grid buffers × 4096 words, all values full-precision |

Upstream scheduling fact re-verified in `sim.js`: the first substep of an `update()` call dispatches zero `g2p2g` workgroups (blank indirect args produced at the end of the previous substep). A single-substep trace therefore has to go through the two-substep frame path.

Stage 2 (unbuilt): Elastic/Sand/Visco seeds, K5/K10, per-substep snapshots, and the committed generator under `scripts/pbmpm/` with its dependency documented.

## A2 — constitutive domains (complete)

Admission (`validProfileDomain` in `Matter.Gpu.cpp`) refuses, before any encoding: non-finite damping constants, non-invertible `dt`, friction angle outside `[0, 90)`, and `exp(1 - plasticity)` that is not finite and positive.

Shader guards, all before the unsafe use and without a second decomposition:
- `solver.slang`: stored `F` → witness `4`; trial `(I + D)F` → witness `8`; checks are finiteness, a derived magnitude bound `1.3e19` (`2·entry² < FLT_MAX`), and determinant/reciprocal/assembled-inverse finiteness. No guard calls `forkSvd`, `exp` or `sqrt`; intentionally limited singular values stay legal.
- `fork_core.slang`, adapter-only blocks: sand records `17` at the zero-deviatoric divide, visco records `16` at non-finite/zero `exp(1 - plasticity)`, `J`, `newJ`. The no-adapter preprocessed text is byte-identical to the pre-change source, so upstream conformance is unaffected.

Tests: `matter_constitutive_gpu` adds three leaves — witness attribution across a liquid reciprocal, stored/trial gradient failures, magnitude and inverse overflow (fields 12/4/4/8/4/4), admission rejection with unchanged committed state, and the transfer identity leaf (partition of unity, first/second moments, affine reproduction). Witness assertions pin `(flags & 8) == 8` and `(flags & 2) == 0` rather than the exact union, because flag `128` is an independent pre-existing stencil-key condition.

Gates after this change: `matter_fast` 21/21 in 291 s.

Build discipline learned here: the fast-gate failure on `app/matter_runtime_gpu/...` after the guard commit was a latent test-ordering bug in that leaf (resolved below), not a build-identity problem. Rebuilding the identical committed sources repaired nothing — the rebuild's green reruns were the same intermittency that later produced 2 failures in 13 runs. The "the staged shader tree did not match the linked binary" explanation recorded at the time was therefore also wrong. Gates and builds still must not overlap while a gate is being measured; that is measurement hygiene, not the cause of this failure.


## A3 — complete iteration schedule

K ∈ {2, 5, 10} now executes the upstream stage sequence instead of a hardwired two.

| Stage | Program | Gather | Constraint + scatter | Integration |
|---|---|---|---|---|
| 0 | `fused0` | no (bootstrap) | yes | no |
| 1 … K−2 | `fused1` | yes | yes | no |
| K−1 | `fused1` | yes | yes | once (gravity, brush, sand pressure feedback) |

The final-stage decision is the runtime predicate `reserved0 + 1 >= reserved1` (`reserved1 = K`); no third entry point exists and no per-K specialization is compiled. Admission rejects every count outside `{2,5,10}` before encoding, allocation or root creation.

Sparse ping-pong is tracked explicitly by the host (`source_a` / `source_generation`) rather than inferred from parity, so odd K consumes the side actually written last. `SparseGridState::m_source_generation` was removed and `constants(destination_a, source_generation)` takes both from the caller.

New shard `engine.tests.app.matter_schedule_gpu`:

| Leaf | Contract |
|---|---|
| `supported_iteration_counts_validate_before_work` | 0/1/3/4/overflow reject with unchanged epoch, accounting and committed rows; 2/5/10 execute |
| `iteration_roles_integrate_once_per_substep` | Integration runs once per substep for K = 2/5/10 (gravity band, sand gradient bound, sand history bound); zero-force rest row is bit-identical |
| `odd_even_schedule_preserves_sparse_sources` | 1/2/8 substeps at K = 2/5/10; run-to-run determinism; a poisoned unused side is never consumed |

Test-contract note: values that cross the fixed-point grid transfer cannot be asserted bit-exact. Gravity uses a 5 % band (transfer quantization ≤ 2.3 % at K = 10 and ≥ 20× below two integrations), sand `F` uses `substep · 1e-5` (from `9/(mass · 1e7)` per stencil), and sand `logJp` uses `1e-4 · previous` with strict decrease (deviatoric transfer noise < 1e-6). Each derivation is in-code and the observed value is logged before the assertion.

## Review disposition

An independent review returned REVISE on the A2 work. Every MUST FIX item was addressed or recorded:

| Finding | Resolution |
|---|---|
| Derived operations still unsafe (visco quotient, sand norm/exponent, coefficient products) | Quotient/argument validation plus a produced-state encodability backstop before scatter; state-driven cases reach witnesses 16 and 17 |
| Liquid and gather reciprocals unguarded | Explicit `1/density` validation; gather `1/volume` is an explicit zero-support branch |
| Magnitude bound rejected usable operands | Replaced with the actual `E²+H²` / `F²+G²` radicand checks; `diag(2e19,1)` admitted, `diag(1e20,1e-20)` rejected |
| Mixed-lane fixture never formed mixed workgroups | Rows now pass discovery and fail inside fused; inverse overflow uses normal-range components; probe finiteness asserted before aggregation |
| Checkpoint CPU contracts outside every gate | New `game.tests.checkpoint_cpu` (19 leaves) in `matter_fast`; also fixed the pre-existing `game/lighting_lab/` exclude that ran 7 leaves twice |
| Transient fast-gate failure assigned an unproven cause | Diagnosed as a test-ordering bug — poll-request completion read as append readiness; fixture fixed, 12/12 versus 2/13 (see below) |
| A0 lacked explicit ceilings | Budgets table above |
| Manifest override precedence ambiguous | Explicit user override versus automatic selection; single status line; stale automatic selection cleared on failure |

### Transient fast-gate failure: resolved as a test-ordering bug

After the constitutive-guard commit, one fast-gate run failed `app/matter_runtime_gpu/resident_completion_keeps_metadata_and_freezes_exact_demand_snapshot` with `device_or_resource_busy` from the append/publication path. The cause is **resolved**: the leaf treated completion of a **poll request** as proof that the GPU append was ready. It called `beginPollEdit` once, discarded the result, and published immediately; when that poll observed "not ready", the runtime correctly left `m_append_prepared = true, m_append_ready = false`, so publication failed the readiness guard. The runtime was right and the test was wrong.

The `device_or_resource_busy` lines in the same leaf are not the failure: they are that leaf's deliberate frozen-snapshot probes, which assert that the demand snapshot stays frozen while a submission is in flight.

The failing interleaving is now prevented by draining the append copy and asserting that the poll returns a readback for the expected identity before publishing. Evidence: 12/12 consecutive runs pass, against 2 failures in 13 runs before. Both the "staged shader tree did not match the linked binary" hypothesis and the reading that the identical rebuild repaired the failure are wrong; the build was never implicated.

The K=5 default change is what made this latent test bug frequent enough to catch: the changed per-tick timing moved the window in which the poll request completes before the append copy is drained. The bug predates K=5 and is not a K=5 regression.

## A4 — iteration-count cost evidence (Release)

Scene: 4096 rows (all four materials by row index), 128×128 grid, `dt = 1/480`, 8 substeps per tick, gravity on, grid-volume channel on, one swept boundary box, no brush. The timed interval covers encode, submit, drain and publication and excludes particle/terrain preparation; the predicted-boundary upload happens inside `submitResidentTick` and is therefore included. 5 warm-up ticks, 30 measured, Release (`clang-rel`), validation layers off.

Scope caveat: this is one pass per K, not the plan's five paired runs, and every sample restarts from freshly replaced state. The numbers are a reproducible-tick cost, not a sustained-gameplay cost.

| K | Wall median (ms/tick) | Wall max | Wall ratio vs K2 | GPU median (ms) | GPU ratio vs K2 | Dispatches/tick |
|---:|---:|---:|---:|---:|---:|---:|
| 2 | 5.36 | 6.46 | 1.00 | 1.89 | 1.00 | 104 |
| 5 | 7.83 | 8.17 | 1.46 | 3.58 | 1.89 | 152 |
| 10 | 12.93 | 13.31 | 2.41 | 6.34 | 3.35 | 232 |

Reading: at 4096 particles one 60 Hz frame is 16.7 ms. K5 costs ~7.8 ms (47 % of the frame), K10 ~12.9 ms (77 %). The GPU stage scales with K (1.89 → 3.58 → 6.34 ms, i.e. 1.89× at K5 and 3.35× at K10) while wall time grows more slowly (1.46× and 2.41×) because the fixed per-tick overhead dominates. Wall growth does not exceed GPU growth at either K.

These are measurements for this scene and host, not product budgets. The leaf is opt-in (`PPR_PBMPM_COST_DIAGNOSTIC=1`, `engine.tests.app.matter_cost_gpu`) and lies outside `matter_fast`; when the variable is unset it logs a line and returns early. In this harness an opt-in leaf that returns early is a *passing* test, so a plain run reports a pass and does not produce a CTest skip.

## A4 — operating-point criteria still open

The cost side is measured; the correctness comparison across K for rest, compression, elastic recovery and visco residual strain is not yet an automated leaf, and the elastic/visco comparison recorded below uses a rotation-sensitive metric that cannot separate stretch from rotation. Cost evidence alone does not justify a default change; the later reference-baseline decision below adopts K5 while retaining these open acceptance questions.

## A4 — correctness matrix across K (Release)

All scenes hold 240 substeps (or the stated horizons), identical initialization per K, exact inventory conservation asserted (mass/volume unchanged for every scene and K). Values are evidence, not contracts.

| Property | K=2 | K=5 | K=10 |
|---|---|---|---|
| Rest state (256-row pool, no gravity): final displacement, rows moved >1 cell | 0, 0 moved | 0, 0 moved | 0, 0 moved |
| Density error on the measured row (ρ=0.75) | 1.67e-6 at s1/8/64 | same | same |
| Elastic response (64-row patch, shear 3), `max \|F − I\|` | 3 → **9941** (s64) → 9986 | 3.31 → **9858** → 9972 | 3.72 → 1.9997 → 2.0001 |
| Visco response (same patch), `max \|F − I\|` | 2.64 → 3.36 → 2.92 | 2.90 → 1.98 → 2.00 | 2.98 → 1.98 → 2.00 |
| Granular settling (8×16 column) | top 17.79, bottom 5.20 | top 20.42, bottom 5.58 | top 21.44, bottom 5.70 |

### Findings

1. **The elastic measurement does not establish elastic recovery either way.** The recorded metric is `max |F − I|`, which cannot distinguish stretch from rotation: `F = −I` is a 180° rotation with zero stretch yet scores 2, so K10's ≈2.0 may be full recovery up to a rigid rotation rather than retained deformation. The K2/K5 values (9941–9986 and 9858–9972) sit under the singular-value clamp ceiling (`Σ ≈ 1e4`) and are consistent with clamp-limited amplification, not with unbounded divergence. What the numbers show is clamp-limited amplification at K2/K5 and a bounded response at K10. Elastic recovery is therefore **undetermined**: it requires measured singular values, `FᵀF − I`, determinant, clamp activation and rigid-alignment shape error. The scene is a prestressed free patch, not a load/unload experiment, so it measures no recovery from an applied load, and nothing "holds" the shear (the earlier "held shear" wording is withdrawn). This remains pre-existing ported behaviour, not a schedule regression, and it is not evidence for or against any K. **Elastic must not be admitted to the game before its constitutive response is determined** (B1's scope).
2. **Rest and compression are unchanged across the measured K, but the sand comparison is neither K-insensitive nor a refinement result.** At rest every K ends with zero final displacement and zero rows moved by more than one cell, and the ρ=0.75 density error on the measured row is 1.67e-6 at substeps 1, 8 and 64. Those are the exact observables: a final displacement plus a one-cell movement count, not accumulated position drift, and a single-row density error, not occupied-volume recovery. Sand settling differs in top height by ~20 % between K2 (17.79) and K10 (21.44), which is too large to call K-insensitive and cannot be called a physical-refinement difference without kinetic settling and support measurements.
3. **Visco is clamped at every K, with different final residuals.** The shear clamp bounds the response at K5 and K10, where the retained `max |F − I|` settles near 2.0 (1.98 → 2.00); K2's final residual is instead 2.92, so the ≈2.0 plateau is not common to every K. The rotation-sensitive metric caveat in finding 1 applies here too.
4. **The single-row compression error is gone after one substep at every K** (25 % error → 1.67e-6). That is a one-row density measurement, not evidence that occupied volume recovered, and it is consistent with the stiffness of the current liquid feedback; it is worth revisiting when B2 replaces the liquid model.

### Operating-point ruling

**Historical ruling, superseded by the reference-baseline decision below: retain K = 2 pending baseline acceptance.** Cost is 1.46× at K5 and 2.41× at K10 measured on 4096 rows. K2 is not a verified acceptable baseline: its correctness side is precisely what baseline acceptance has still to establish, and the metric used here leaves the elastic and visco responses undetermined. These measurements alone do not justify switching defaults, and K10's bounded elastic response is not a fix.

## A4 — re-measured with rotation-free metrics

The earlier recovery metric (`max |F − I|`) was withdrawn as rotation-sensitive. The diagnostic now reports singular values, `‖FᵀF − I‖_F`, `det F`, clamp activation counts, kinetic energy and positional offset, and applies a brush load for 16 substeps followed by a 224-substep release.

Identity: Release binary sha256 `269bf20f…`, source `078f48a` plus the metric edit; GPU **not** exclusive (desktop compositor and a clock ramp present throughout), so cost variance of ~10 % applies.

| Property | K | horizon | σmax | σmin | ‖FᵀF−I‖ | clamp rows | Σ KE | offset |
|---|---:|---|---:|---:|---:|---|---:|---:|
| elastic load/release | 2 | load_end | 1.016 | 0.944 | 0.109 | 0 | 1.72e4 | 0.403 |
| elastic load/release | 5 | load_end | 1.002 | 0.992 | 0.0165 | 0 | 1.72e4 | 0.366 |
| elastic load/release | 10 | load_end | 1.001 | 0.997 | 0.0067 | 0 | 1.71e4 | 0.363 |
| elastic load/release | 2 | release_end | 1.001 | 0.9995 | 0.0015 | 0 | 1.71e4 | 11.16 |
| elastic load/release | 5 | release_end | 1.000 | 1.000 | 6.4e-5 | 0 | 1.67e4 | 11.07 |
| elastic load/release | 10 | release_end | 1.000 | 1.000 | 5.7e-5 | 0 | 1.63e4 | 10.99 |
| first-step relaxation | 2 | s1 | 3.55 | 0.284 | 11.6 | 0 | 1.30e6 | 1.01 |
| first-step relaxation | 5 | s1 | 3.54 | 0.100 | 11.6 | **10** | 1.07e7 | 2.14 |
| first-step relaxation | 10 | s1 | 4.15 | 0.100 | 16.2 | **6** | 3.06e7 | 3.47 |
| granular state | 2 | settle_end | 1.000 | 0.738 | 0.457 | 124 sand | 2.72e4 | 17.7 |
| granular state | 5 | settle_end | 1.000 | 1.000 | 1.1e-6 | 128 sand | 6.54e4 | 15.2 |
| granular state | 10 | settle_end | 1.000 | 1.000 | 9.8e-7 | 128 sand | 6.02e4 | 14.8 |

Cost re-measured (non-exclusive GPU): K2 5.91 ms, K5 8.39 ms (1.42×), K10 13.59 ms (2.30×) wall per tick; GPU 1.82 / 3.36 / 6.91 ms; dispatches 104 / 152 / 232 as designed.

### What this establishes

- **Recovery ratio is not comparable across K.** The applied load differs by 16× between K2 and K10 (`0.109` vs `0.0067` at `load_end`), so K5/K10's smaller release residual means less distortion was applied, not better recovery. Within a K, the retained distortion falls to ~1 % (K2) and ~0.4–0.9 % (K5/K10) of the applied value, and positional offset keeps growing — the shape relaxes while the body keeps translating. `‖FᵀF−I‖` is blind to translation/rearrangement and inflated by near-singular elements, so it cannot mean "returned to the initial shape".
- **The load path is itself K-dependent**: the same 16-substep brush produces 16× less distortion at K10 than at K2.
- **Higher K drives first-step trials into the integration clamp** (10 rows at K5, 6 at K10, none at K2) with `σmin` pinned at the 0.1 floor and `det F` down to 0.043 — near-inverted trial states that K2 does not reach.
- **Sand is qualitatively K-dependent**: K2 retains distortion while K5/K10 project every row and report ~1e-6.
- The load/release fixture does not separate elastic from visco (yield clamp never active); only the first-step fixture does.

### Operating point

**K = 5 is now the new-world default by reference-baseline decision**, superseding the earlier retain-K2 ruling: the reference implementation's baseline scenes use five iterations per substep at the same 480 Hz timestep. `kDefaultIterations = 5u` names that policy; `kSourceIterations = 2u` remains the pinned no-adapter conformance default. K2 and K10 remain admitted and selectable. Per-world persistence is unchanged: v6 restores K2 and v7 restores its saved schedule, including K10.

This decision does not establish baseline acceptance or erase the measured caveats: K5 costs 1.42× K2 in the non-exclusive re-measurement; the same brush load produces 16× less applied distortion at K10 than K2 (not a 16× K5 result); K5 activates the first-step integration clamp on 10 rows; and sand is qualitatively different across K. These remain unresolved model questions, not demonstrated improvements, and Elastic admission still requires its constitutive response to be understood.

Follow-up: re-derive the liquid relaxation constant for K5 instead of inheriting the K2 stability tuning, and investigate the first-step clamp activation. This default change leaves liquidRelaxation, elasticRelaxation, sampling, units and the fixed-point multiplier unchanged. Build and runtime validation of the default change remain with the orchestrator.

## A2 — precision and quantization characterization

**Measurement:** 148 reports in one Release (`clang-rel`) run, 31.2 s total, K = 5 with a K = 2 control, zero gravity, brush and contacts, volume feedback off.

| Dimension | Values |
|---|---|
| Coordinates (cells) | 6.5 / 512.5 / 1024.5 / 4096.5 |
| Subcell phase | 0, 0.5 |
| Speed (m/s) | 0.001 / 0.01 / 0.1 |
| Normalized mass | 1 / 0.01 / 0.0001 |
| Horizon (substeps) | 1, 480 |

Rows are single particles or affine patches with no contacts, so the measurement isolates transfer precision from constitutive and contact behaviour. Release is deliberate: the Debug/validation configuration's arithmetic is not the configuration these limits will be quoted for.

### Findings

1. **Fixed-point dissipation is cumulative transfer quantization, not a unit error.** One substep reproduces 99.6 % of the seeded displacement, which proves the cell and m/s conversion is correct. Repeated quantized transfers then compound: a 0.1 m/s, unit-mass particle travels 0.1004 cells against an ideal 0.3000 over 480 substeps. Each of the 480 quantized P2G/G2P round trips truncates again, so the loss is in the transfer, not in the initial encoding.

2. **Absolute FP32 positions freeze positive increments below half a position ULP.** The ULP of a position near 512.5 cells is `6.1e-5` cells, near 1024.5 cells `1.22e-4`, and near 4096.5 cells `4.88e-4`; those correspond to roughly 0.0049 / 0.0098 / 0.039 m/s. The observed freeze follows those thresholds: mass 1 at 0.001 m/s moves at coordinate 6.5 but freezes at 512.5; at 0.01 m/s it moves at 512.5 but freezes at 1024.5; at 4096.5 it stays frozen while still reporting nonzero displacement. 102 of 146 single-particle rows froze. The cause is absolute-world FP32 storage, so the reachable slow-motion range shrinks as the world coordinate grows.

3. **Initial P2G scatter has a velocity floor that depends on weight and particle mass.** Node momentum encodes as `trunc(M·w·m·d)` with 2D weights up to 0.5625 (centre node) and 0.375 (halfway node), so the floor is `1.6e-5/(w·m)` m/s — `2.8e-5/m` for the centre weight and `4.3e-5/m` for the halfway weight. At normalized mass `1e-4` every tested speed is below that floor.

4. **The smallest-mass row also crosses the node mass support cutoff.** That case reaches `mass < 1e-5` at a corner node (`1.6e-6`), so its result measures the scatter floor and the support cutoff together, not the two loss channels in isolation. The fixture was corrected and the combined effect is recorded as such rather than attributed to either channel alone.

5. **The affine result is end-to-end liquid shear decay, not APIC retention.** A symmetric shear decays to zero over 480 substeps. Because every stage runs its constitutive relaxation, the correct reading is the total liquid shear decay of the update. It is not a measurement of affine particle-grid retention: the earlier `(1−viscosity)^N` reference was an unjustified model for this pipeline, and the metric was renamed to say what it measures.

### Limits to carry forward

- These are engine limitations to record, not production fixes to make in A6.
- The leaf asserts no motion magnitude; a green exit code establishes that the structure runs and stays finite, not that motion is correct.
- The supported motion range remains an **open question**: slow motion below the half-ULP floor is lost, and quantization costs about 0.4 % per substep at 0.1 m/s. Neither limit is characterized as acceptable for any gameplay range yet.

## A5 — sand history derivation

`docs/pbmpm-sand-history-derivation.md` is the A5 gate artifact. Its conclusions, in the terms it states them:

- The candidate conserves the `Σ log σ + H` ledger on the unclipped domain; the identity holds in every return branch, but only for that constitutive return.
- Dissipativity needs explicit constitutive assumptions: `μ > 0`, `K₂ > 0`, `c ≥ 0`, `H ≥ 0`, and the Hencky energy with no extra stored energy in the history. It is not unconditional, and negative imported history breaks it.
- `elasticityRatio` is not justified as a modulus ratio. In `constraint` it is a blend weight between a singular-value target and the determinant-normalized target, not a measured `λ/μ`; the yield coefficient needs an explicit or calibrated value instead.
- The `logJp == 0` constraint branch interacts: for every `t < 0` output the clamp necessarily changes at least one singular value, so the ledger cannot be assumed unchanged across the combined constraint/return step.
- Implementation is **blocked pending a plan-owner decision** on the yield coefficient (effective 2D versus true plane strain, friction calibration, and the modulus ratio or calibrated `c`) and on the physical meaning and admissible domain of `H`.

No A5 code was written: the artifact is analysis only, with no shader, host, test or build change.

## A6 — acceptance at frozen identity

Identity: HEAD `5355f3ee9716d77cdeecb9feca6e0a74b4fefcd2`; shader source set sha256 `4154830028c2a0a634c9144d03be466237efb451c7fc2a33c1fb5207da225b0b`; `engine.tests.app` dev `bcbfbdf666c55795`, Release `26a0cfc50e4978a7`; `game.tests` `2acf41eee25c2955`. GPU utilisation 6 % before and 19 % after (no competing compute).

| Run | Result | Wall |
|---|---|---:|
| `matter_fast` #1 | 24/24 passed | 316 s |
| `matter_fast` #2 | 24/24 passed | 315 s |
| `matter_fast` #3 | 24/24 passed | 330 s |
| `matter_long` | 2/2 passed | 2378 s |

All four runs inside their ceilings (fast ≤ 420 s, long ≤ 3120 s). Format check clean; semantic style 0 blocking on all 16 changed C++ files. Both presets build (`clang-dev`, `clang-rel`).

**Phase A status at this identity**

| Task | State |
|---|---|
| A0 suite census, gates, budgets | complete, green |
| A1 upstream harness | stage 1+2 harness complete and deterministic **outside** the repo; in-repo generator and K5/K10 conformance fixtures not yet provided |
| A2 constitutive domains + precision | complete; precision limits characterized (see above) and the supported motion range remains open |
| A3 schedule + persistence | complete; K ∈ {2,5,10}, envelope v7, fresh worlds K5 |
| A4 operating point | decided: K5 by reference-baseline decision, caveats recorded |
| A5 sand history | **blocked pending a plan-owner decision** (yield coefficient convention and the admissible domain/meaning of `H`) |
| A6 acceptance | Linux/Debug+Release gates green at the frozen identity; **Windows/D3D12 unrun** |

Not claimed: upstream K5/K10 conformance, gameplay-range acceptance for the precision limits, A5 correction, and cross-platform validation.
