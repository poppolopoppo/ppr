# PB-MPM baseline and multi-layer results

**Plan:** [PB-MPM baseline correction and multi-layer plan](../../.opencode/plan/2026-11-15-pbmpm-baseline-and-multilayer.md)
**Status:** Phase A in progress. A0 complete; A1–A6 pending.
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

- A1 stage 2 (Elastic/Sand/Visco, K5/K10, N-substep snapshots, committed generator), A2's precision diagnostic, and A3's checkpoint persistence are not started or incomplete; see the sections below.
- D3D12/Windows verification is unavailable on this host and remains an unrun gate.
- Only the Debug/ASan/validation configuration has been measured; Release timing and cost evidence is pending.

## Mandatory gate budgets

| Gate | Entries | Measured | Ceiling |
|---|---:|---:|---:|
| `matter_fast` | 23 | 291 s (21 entries, exclusive host) and 574.6 s (23 entries, concurrent-session GPU load) | **≤ 420 s exclusive** |
| `matter_long` | 2 | 961 s (exclusive host) | **≤ 1200 s exclusive** |

Ceilings are task-local repeated-use budgets for this reference host and Debug/validation configuration; they are not product budgets. A measurement taken while another session holds the GPU is recorded but does not establish the ceiling. Both gates are green at this source state.


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

Build discipline learned here: a fast-gate failure on `app/matter_runtime_gpu/...` after the guard commit disappeared when the identical committed sources were rebuilt, and the gate then passed. The build had been taken while an implementation lane was still editing `assets/shaders/pbmpm`; the staged shader tree did not match the linked binary. Builds must follow a quiescent lane.


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
| Transient fast-gate failure assigned an unproven cause | Recorded below as unresolved |
| A0 lacked explicit ceilings | Budgets table above |
| Manifest override precedence ambiguous | Explicit user override versus automatic selection; single status line; stale automatic selection cleared on failure |

### Unresolved incident

After the constitutive-guard commit, one fast-gate run failed `app/matter_runtime_gpu/resident_completion_keeps_metadata_and_freezes_exact_demand_snapshot` with `device_or_resource_busy` from the append/publication path. A rebuild of the identical committed sources passed 3/3, and both gates have been green since. The cause is **not established**: the failing run's binary and staged shader identities were not captured, so the earlier "staged shader mismatch" explanation is withdrawn. It is retained as an unexplained transient until a reproduction provides identities.

## A4 — iteration-count cost evidence (Release)

Scene: 4096 rows (all four materials by row index), 128×128 grid, `dt = 1/480`, 8 substeps per tick, gravity on, grid-volume channel on, one swept boundary box, no brush. Excludes allocation/upload; the timed interval covers encode, submit, drain and publication. 5 warm-up ticks, 30 measured, Release (`clang-rel`), validation layers off.

| K | Wall median (ms/tick) | Wall max | Ratio vs K2 | GPU median (ms) | Dispatches/tick |
|---:|---:|---:|---:|---:|---:|
| 2 | 5.36 | 6.46 | 1.00 | 1.89 | 104 |
| 5 | 7.83 | 8.17 | 1.46 | 3.58 | 152 |
| 10 | 12.93 | 13.31 | 2.41 | 6.34 | 232 |

Reading: at 4096 particles one 60 Hz frame is 16.7 ms. K5 costs ~7.8 ms (47 % of the frame), K10 ~12.9 ms (77 %). The GPU stage scales roughly with K (1.89 → 3.58 → 6.34 ms) while wall time grows more slowly at K5 because the fixed per-tick overhead dominates; K10's wall growth exceeds its GPU growth, consistent with the larger dispatch count and per-stage encoding.

These are measurements for this scene and host, not product budgets. The leaf is opt-in (`PPR_PBMPM_COST_DIAGNOSTIC=1`, `engine.tests.app.matter_cost_gpu`), outside `matter_fast`, and logs an explicit skip otherwise.

## A4 — operating-point criteria still open

The cost side is measured; the correctness comparison across K for rest, compression, elastic recovery and visco residual strain is not yet an automated leaf. Until it exists, the K5 default must not be adopted on cost evidence alone.
