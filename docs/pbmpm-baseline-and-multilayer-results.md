# PB-MPM baseline and multi-layer results

## B1 increment 2 — historical tested identity; review revision unrun

Coordinator R1–R5 decisions are implemented in the adapter path; see
[material-law decisions and risks](pbmpm-material-laws-proposal.md). Historical
Phase A results below do not validate this edit-only revision. The coordinator
reports prior gates at `a98f399d`; the present working-tree review changes are
not part of that tested identity. New-law acceptance is **authored-analytical**,
with no upstream oracle. The exact prior command lines and external gate ledger
must still be supplied by the validation owner; they are not inferred from test
names. Available historical logs `/tmp/opencode/b12-t2.log` and
`/tmp/opencode/b12-fast2.log` record CTest output under `out/build/clang-dev`,
but do not contain the invocation or independently bind binaries to the commit.

* R1: dimensionless log-strain calibration space, inventory-only density; new
  families use H=0 and separate C/phi. Risk: no stress/stiffness mapping.
* R2: constant-dev cap/tension clamps, constant-t non-associated shear return,
  monotone hardening. Only metric non-expansiveness, active n reduction and
  monotone history are claimed. No dissipation proof or work-sign claim because
  R1 supplies no stress-space mapping. Risk: sequential empirical return.
* R3: explicit porous ellipse / dense cone, phi increases by beta times removed
  over-compression. Epsilon is numerical. Only the t=0 edge is continuous;
  finite-compression edges intentionally jump. Coordinator accepts that risk.
* R4/M1: authored affine-velocity drive, nominal 1% shear over 0.25 s, zero
  gravity/no contacts, K5, then remove only the driving velocity/D contribution
  and release for 2 s. Per-substep maximum `||F^T F-I||` and rigid-aligned
  positional RMS/initial RMS radius must both remain `<1e-3` for the final
  0.5 s. Nominal shear norms ~1.41e-2/~5e-3 justify these bounds. Drive peaks,
  endpoint values and final-window maxima are reported; insufficient excitation
  cannot establish recovery. Outcome is report-only; rock/ore remain granular.
* R5: fresh/dug C=0, phi=phi0; semantic96/hot72/cold28, property76;
  checkpoint v9/policy2. v6–v8 preserve old words/families and default extensions.
  Risk: old saves deliberately retain their old family behavior.

Authored coverage: cap consumption, monotone hardening, zero-confinement
cohesion, tension, phi compaction/release, dense branch/zero-confinement edge
continuity, invalid admission, clay/sand divergence, snow strengthening,
checkpoint exact state/migration, reflection/packing. New CTest entries:
`engine.tests.app.matter_calibrated_laws_gpu` and
`engine.tests.app.matter_elastic_recovery_diagnostic`. Renderer ABI fixtures
were updated as dependent consumers. No builds or tests were run by the
implementer for this review revision. This is not a GREEN result.

Review edits: C1 freezes C/phi and their strengths when projecting the trial
constraint target, retaining the Q blend and relaxation; M2 accepts historical
88-byte rows after checking their original file hash. Added coverage exercises
porous-to-dense return with shear and non-default history replacement/append,
including exact dense phi. M1 and loaded S1 scenes are **authored, unrun coverage**,
with no pin/weld seam. Prior tested source identity remains `a98f399d`
(coordinator-reported gates), not this working tree. Exact historical validation
commands remain unavailable here. No new measured numerical values are claimed.

S1 (`engine.tests.app.matter_calibrated_loaded_diagnostic`) uses production default
law coefficients and gravity/contact-only loading of a 4-by-12 column on a box
floor, dt=1/480, K5. Clay versus a granular-family control reports tagged layer
height, width, floor-relative support height and differences after 1 s. Snow
reports mean phi and geometry at 0.25 s intervals through 1 s compression, 1 s
zero-gravity release, and 1 s equal-gravity reload. Phase-boundary solver restarts
preserve and byte-compare every particle word. Increased phi retained after
release measures retained compaction; retained geometric shortening is also
reported. Smaller additional normalized axial shortening on reload than first
load reports increased structural resistance, conditioned on positive first
shortening and retained phi. This geometry-dependent compliance proxy is not a
stress modulus. Outcomes are report-only; **no run values or successful behavior
verdicts exist yet**.

S2 ruling: **v8 migration evidence is reconstruction-based, shared with the CPU
mirror; no independent historical v8 artifact exists in the repository.** Start
from v9 water/default rows: table offset `L-S-8-(16+76*M)`, particle offset
`table-96*N`, schedule offset `particles-72`. Remove the table, trim bytes [88,96)
from particles in reverse order, insert policy1/64-bit FNV-1a identity/count plus
first ten words per property. Identity hashes little-endian policy/count/ID-sorted
ten-word rows. Retain K/profile/snapshot and set byte 8 to 8. The helper and added
GPU assertion check `schedule_offset+4+64+4+88*N+16+40*M+8+S` before decoding.
This does not supply historical clay/snow or custom-coefficient evidence. The
independent artifact is deferred; a future genuine artifact can replace the
fixture. Frozen evidence is retained and no payload was invented.

All current-revision builds, shader checks, M1/S1 executions, checkpoint assertion,
Linux/Windows compatibility and performance validation remain unrun by this
edit-only author and belong to the coordinator's validation lane.

---

**Plan:** [PB-MPM baseline correction and multi-layer plan](../../.opencode/plan/2026-11-15-pbmpm-baseline-and-multilayer.md)
**Status:** A0, A2 (constitutive and precision), A3 and A4 complete; A1 generator and checkpoint leaves landed with bounded comparisons and structural-only exclusions detailed below; A5 closed as a documented rejection for Phase A, characterization executed, reconsider at B1. A6 Linux gates passed at the recorded frozen identity; the post-A1 exclusive fast-gate budget and Windows/D3D12 remain unverified. The pre-merge branch-row policy revision is edit-only and awaits orchestrator validation.
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
- A1 stage 2 and the committed generator have landed; numerical coverage exclusions and the deferred differential trace remain open. A2 precision is characterized but its supported-motion-range question remains open; A5 is rejected for Phase A and reopens at B1. See the sections below.
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

Stage 2 subsequently landed: Elastic/Sand/Visco seeds, K5/K10, per-substep upstream snapshots, and the committed generator under `scripts/pbmpm/` with its dependency documented. Native checkpoint execution and its coverage limits are recorded below.

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

## A5 — documented rejection for Phase A; reconsider at B1

[Sand history derivation](pbmpm-sand-history-derivation.md) is the A5 gate artifact. Its original blocked recommendation is superseded by this plan-owner disposition: **A5 is closed as a documented rejection for Phase A; the current law is retained.** Its analytical conclusions remain:

- The candidate conserves the `Σ log σ + H` ledger on the unclipped domain; the identity holds in every return branch, but only for that constitutive return.
- Dissipativity needs explicit constitutive assumptions: `μ > 0`, `K₂ > 0`, `c ≥ 0`, `H ≥ 0`, and the Hencky energy with no extra stored energy in the history. It is not unconditional, and negative imported history breaks it.
- `elasticityRatio` is not justified as a modulus ratio. In `constraint` it is a blend weight between a singular-value target and the determinant-normalized target, not a measured `λ/μ`; the yield coefficient needs an explicit or calibrated value instead.
- The `logJp == 0` constraint branch interacts: for every `t < 0` output the clamp necessarily changes at least one singular value, so the ledger cannot be assumed unchanged across the combined constraint/return step.
- Replacement requires decisions on the yield coefficient (effective 2D versus true plane strain, friction calibration, and the modulus ratio or calibrated `c`) and on the physical meaning and admissible domain of `H`. Phase A does not make these constitutive choices implicitly.

The retained limitation is exact: at zero deformation, positive history obeys
`H_n = H_0 * 2^-n`, so `logJp` is **not conserved plastic log-volume**.
`eHat = log(Sigma) - (sum(log(Sigma)) + H)/2` has trace `-H`, not zero.
The measured sand response is K-dependent: the original settling top heights
were 17.79/20.42/21.44 at K2/K5/K10; the rotation-free measurement retains
distortion at K2 while K5/K10 project every row (see A4). Neither result
establishes physical refinement or sand correctness.

`app/matter_gpu/sand_history_phase_a_characterization` now pins the retained
law through the production resident path: isolated positive histories 0.2 and
1.0 halve exactly for eight substeps at K2/K5/K10, with the committed `F`
remaining identity within the transfer allowance. The internal `eHat` is not
read back: the observable assertion is the committed `(F, logJp)` pair and
its ledger loss, not a direct measurement of the internal trace. An anisotropic
`F=diag(1,0.5), H=0.2` case pins the current projection and rejects the
candidate's conserved ledger. Three-substep ticks also pin three halvings but
only one epoch advancement per tick. The leaf is registered exactly once in
`matter_material_gpu` (`matter_fast`). It was included in the recorded post-A1
24/24 fast-gate pass below; no shader or host law was changed.

The conformance divergence motivates investigating replacement as a hypothesis,
with repair of a transcription defect or a compiler difference still open.
Sand K10 `D.c0x` diverged by 2.34e-3 against a 7.1e-4 cap and its comparison
is structural-only. Matching final-checkpoint predicates cannot establish
earlier branch history; the deferred differential trace must resolve the cause.

**Reopen at B1, before sand property semantics are frozen, or earlier on a
demonstrated supported-workload failure.** The gate artifact above and the
differential trace are inputs to that decision. Success here means the
characterization catches a silent change; it does not certify the retained law.

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
| A5 sand history | **closed as a documented rejection for Phase A; current law retained, reconsider at B1** (characterization added after this frozen identity and included in the post-A1 fast-gate pass below) |
| A6 acceptance | Linux/Debug+Release gates green at the frozen identity; **Windows/D3D12 unrun** |

Not claimed: upstream K5/K10 conformance, gameplay-range acceptance for the precision limits, A5 correction, and cross-platform validation.

## A1 executable checkpoint fixtures

**Executed under the limited comparison policies below.** Twelve homogeneous and three
interacting mixed-family reference cases are encoded in
`lib/engine/tests/app/App.MatterFork.Upstream.h` and consumed by five new
`App.MatterFork` leaves. The mixed upstream traces were generated offline; the
recorded post-A1 gate passed, but its exclusive timing budget remains unverified.
The pre-merge structural-only branch-row revision has not been executed here.

### Provenance and offline reproduction

The producer is `scripts/pbmpm/stage2.mjs`, with the compute-only seed/copy
instrumentation in `scripts/pbmpm/src/gpu.js`. Its upstream WGSL is pinned to
`61749b6c2d2631ed243f86882752ba43196f2de7`; see the vendored generator README for
runtime installation and invocation. Generation is **offline and never part of
CTest, matter_fast, builds, or other gates**. Tests consume only compiled word
arrays and do not require Node, Dawn, network access, or the external trace tree.

This extraction consumed `/tmp/opencode/pbmpm-harness/out/stage2/index.json` and
its twelve case files. Each file's SHA256 was checked against the index before
extraction. Each case has 64 particles and eight genuinely simulated substeps;
`meta.verification.allStepsDidWork` was required. The producer's per-dispatch
`workProof` distinguishes its zero-work bootstrap substep from the K nonzero
dispatches in each reported substep. Producer: Node 22.23.5, webgpu 0.6.2/Dawn,
RTX 2080/Turing, NVIDIA 620.70.06.

#### Interacting mixed-family extension

The mixed seed has 36 rows, in y-major/x-minor order within the same (8,8)–(16,16)
footprint: 16 liquid centres at x=8.5..11.5, y=8.5..11.5; 16 sand centres at
x=12.5..15.5, y=8.5..11.5; four elastic centres at x=9.5..10.5, y=12.5..13.5.
Spacing is one cell, including across both interfaces. Quadratic support has
radius 1.5 cells per axis (3×3 nodes): the liquid/sand boundary rows share node
columns 11 and 12, and liquid/elastic rows share node rows 11 and 12 with positive
weights. Thus they exchange momentum through the same grid, rather than evolving
as isolated blocks. All rows use the homogeneous cases' velocity field, gravity,
mass, volume, F, D, J and history initialization and parameter profile.

Offline invocation from the checkout root (the existing external dependency
installation is selected by NODE_PATH; no install or network access was needed):

```sh
NODE_PATH=/tmp/opencode/pbmpm-harness/node_modules node scripts/pbmpm/stage2.mjs \
  --families mixed --iterations 2,5,10 --steps 8 --out-dir /tmp/opencode/a1-mixed/run-a
NODE_PATH=/tmp/opencode/pbmpm-harness/node_modules node scripts/pbmpm/stage2.mjs \
  --families mixed --iterations 2,5,10 --steps 8 --out-dir /tmp/opencode/a1-mixed/run-b
```

These use vendored `scripts/pbmpm/upstream` unchanged. Two separate processes
produced byte-identical case files (and index); timings are intentionally excluded.

| Mixed case | JSON SHA256 |
|---|---|
| mixed-k2-s8 | `1a59048d0694a3a5bd8db60fa853c20401dd58a2b078e953e7bffcd8d569455d` |
| mixed-k5-s8 | `0f18983c5c8f73c5a3bd55eb31dd4127241c740d8ff27f0fa589b192c72dd3b6` |
| mixed-k10-s8 | `edc935eb40c5916d9d6c5fcd1326b3ef96847f5fc6b8c63ae9449d484606f2b3` |

For every K, `workProof` has eight entries: each entry has K bootstrap zeros,
followed by K workgroup counts of 3 for steps 0–2 and 4 for steps 3–7.
`bootstrapAllZero`, `simulatedAllNonZero` and `allStepsDidWork` are true.
Finite/frozen-field checks and the separate liquid, elastic and sand signatures
also pass for each mixed trace.

Regression check: all twelve homogeneous cases were regenerated using the vendored
driver and `PBMPM_UPSTREAM=/tmp/opencode/pbmpm-upstream2` into
`/tmp/opencode/a1-mixed/homogeneous-original-path`; `diff -q` against each original
`/tmp/opencode/pbmpm-harness/out/stage2/*-s8.json` succeeded with no differences.
The override preserves the original trace's provenance-path string. A separate
vendored-default run in `homogeneous` also matched every step, but its JSON differs
in that pre-existing provenance-path convention; no metadata normalization was
used for the byte-identical check. The default generator still selects only the
original four families; mixed is explicit opt-in.

Extraction is bit transport, not a second solver: round each JSON number to
IEEE binary32, reinterpret as u32, take `meta.seed.rows`, and project
`steps[7].particleWords` with the hot/cold index lists below. All 64 homogeneous
or 36 mixed rows are kept.
The four family seeds are identical across K; all families share the full liquid
seed except material/RGB word substitutions. The exporter checked that fact for
every seed word. It also checked that every final cold word equals its seed word,
so the expected cold array aliases the seed projection instead of repeating it.
The temporary extraction script for this handoff is
`/tmp/opencode/export-a1-fixtures.py`; it is not a gate dependency.

The generated header is **199,279 bytes (194.61 KiB)**, SHA256
`04c565d79b77ed11875dba5d3740e60bb20d80e06c900d40f393882e49dfa847`.
This is +18,400 bytes over the homogeneous header, with 5,521 bytes remaining
under the 200 KiB budget. Numeric tables retain eight words per line under
localized clang-format markers; redundant spaces after commas were dropped to
fit the mixed seed and three final checkpoints. No case, row or semantic field
was dropped. Mixed seeds are shared across K and checked against all three traces.
Including substep 1 hot words would add approximately 162,080 bytes before
formatting, exceeding the approximately 200 KiB budget. Consequently only
substep 8 is stored: this detects final divergence but loses early-stage
localization and transient divergences that reconverge before substep 8. The old
frozen fixtures and their full stage comparisons remain intact.

Each homogeneous logical fixture is 11,776 bytes: 64 × (24 seed + 16 expected hot + 6 expected
cold) × 4. The fixture hash below covers that concatenation, each array in row
order and each word encoded little-endian, independently of header formatting
and source-level deduplication. Each mixed fixture is 6,624 logical bytes;
total logical size is 161,184 bytes (+19,872).

| Case | Original case JSON SHA256 (from index.json) | Logical fixture SHA256 |
|---|---|---|
| liquid-k2-s8 | `b38d0a0755111580fa2bfd490f6dc693ffe7b4cac4733668f2d66da01330db70` | `257866b23b21161ccef50cc24a7daca5f4e6e6fd1bc183d0c8ec29e2c710befa` |
| liquid-k5-s8 | `220bd4e37777c89f4f6686321b70bd6dddac353fe53bc22f346702cfb54cd051` | `3ae4ea2df2fb625defb1150a0c69b313b70df1b1468164f9884a4aebd190e642` |
| liquid-k10-s8 | `fcfc4e8b3971c3aa3eb454a8b6a00f1e059181045afbd3432d389f05f2fbb95a` | `3fd6f1faed6a3733c004f0d5222651c7e49188e9f29214c47509ee60069ae092` |
| elastic-k2-s8 | `d30f74a374020e5f8f5b77ce4b9d20ca7b29e78619ebebb84d7ee907412752e4` | `7acee302c15309a7e9f5e87ba3f2640a82999ff3292dee553e20bc5f0d2b0e15` |
| elastic-k5-s8 | `59adf0327c8f6d892febae39865913fc6c6baca6fe409facecc65f1fc2044ad8` | `0141c8af444bc43b55c7b8c09b45c854de33c51b416d3a05414cbd0da549fa8a` |
| elastic-k10-s8 | `44eb8aea49c0b0801c2343a77ee73eeb984bb849ba1fae1618ab8bb4cbb3e5d7` | `9923e8005b48cd9c9caa498255a9bad53b67caf93ddb08d18b5d560728578749` |
| sand-k2-s8 | `6651e6dda4cbfdb33e493362eb26b84d9287072a1730fd2402a0d016115e6a5f` | `32b529dc2754ad33e558ca772bf322e765d2ead7fc0ab981b2a379ec580eb5ff` |
| sand-k5-s8 | `bca5c0cf484338fd15aa3043afdaf2caf0e89b4a778a187c262d758d4234928f` | `6267da10eec8b0bba83f0d83db6c9473ce79956672ed3f4c4a3eb1500290bc42` |
| sand-k10-s8 | `abf6a0a12256315d7cf15503e357c5271c645eb032bef3eb0dab05147cfa4bda` | `7471bfbb274a88608e6a6d00854a6aa8f13603523f3ff890955a7b4b6fc2b772` |
| visco-k2-s8 | `05919d905c154835790d89ad0127609e5db484162c592b33897ab74192f566d4` | `877bd1130a3a661d4ff46b4b7833de228206d2d3ac4bdfb46ece22a314db438f` |
| visco-k5-s8 | `01f2e293ce16e00470b10a590b809bcd19485a2b4e5c8898510479f69940160f` | `8daf64b3c8f05b9d124cd7687a5102b6085e0b122cb53602e8b240c91f0bc2b9` |
| visco-k10-s8 | `841c519ba07da2274bbb42503c2b8e0ae64f12d285e5a7117501eecd571cf962` | `05ab3301f9816e546c74a1b03d5fa7f70a5c48fa0f2f496aa9f9262b192c2376` |

### Exact storage mapping

Offsets below are 32-bit **word** indices. Every semantic word remains FP32;
material and enabled are not integer-reinterpreted enumerators. F and D retain
WGSL column order, with no transpose, axis flip, unit scaling, or remapping.

| Upstream word(s) | Meaning | PPR device word(s) |
|---|---|---|
| 0, 1 | position x, y | Hot 0, 1 |
| 2, 3 | displacement x, y (cells/substep) | Hot 2, 3 |
| 4, 5, 6, 7 | F: col0.x, col0.y, col1.x, col1.y | Hot 4, 5, 6, 7 |
| 8, 9, 10, 11 | D: col0.x, col0.y, col1.x, col1.y | Hot 8, 9, 10, 11 |
| 12 | liquidDensity / J | Hot 12 |
| 13 | mass | Cold 0 |
| 14 | material (liquid=0, elastic=1, sand=2, visco=3) | Cold 2 |
| 15 | volume | Cold 1 |
| 16 | lambda | Hot 14 |
| 17 | logJp | Hot 13 |
| 18, 19 | vec3 alignment padding, zero in seeds | omitted; restored as zero |
| 20, 21, 22 | color r, g, b | Cold 3, 4, 5 |
| 23 | enabled | Hot 15 |

No upstream semantic particle field is dropped. PPR omits only the two upstream
padding words. `State` internally uses the older eight-word oracle-cold
transport (padding at 3 and 7), then compacts it to the actual six-word Cold
buffer; those two compatibility padding words are not device data. Each case
uploads its seed, reads both device buffers back, restores all 24 upstream words,
and asserts bit-for-bit equality before dispatch. This GPU round-trip assertion
was exercised by the recorded post-A1 execution.

### Matched profile and execution path

| Parameter | Conformance value |
|---|---|
| Domain / rows | 32 × 32 / 64 homogeneous, 36 mixed |
| dt | binary32(1/120) seconds |
| Fixed-point multiplier Q | 10,000,000 |
| Gravity strength | 2.5 (source applies gridHeight × gravity × dt²) |
| Liquid / elastic relaxation | 1.5 / 1.5 |
| Elasticity ratio | 1 |
| Liquid viscosity | 0.01 |
| Friction angle | 30 degrees (source conversion uses 3.14159) |
| Plasticity | 0.9 |
| Grid-volume liquid channel | enabled (1) |
| Border friction | 0.5, already hard-coded in the no-adapter shader |
| Iterations / substeps | K=2,5,10 / 8 |
| Shapes, terrain, bodies, mouse/radial force | absent |

The seed has F=identity, D=zero, J=mass=volume=enabled=1, lambda=logJp=0.
Its 8×8 cell-centre block starts at (8,8); displacement is
`(0.02*(x-12)+0.30*(y-12), 0.02*(y-12))`, rounded once to binary32 by the producer.
The test does not recompute that field. Production defaults instead use dt=1/480,
liquid relaxation=1, plasticity=0, and `GpuParticle` defaults logJp=1; none of
those defaults may replace the explicit upstream profile/seed here. The harness
rhoZeroMultiplier is not a SimConstants member and does not reach either GPU.

The new leaves use `Fixture`/`State` and `fork_conformance.slang`, which includes
`fork_core.slang` without `PBMPM_ENGINE_ADAPTERS`. They do not use the game adapter
or the production `solver.slang` path. The latter has a runtime final-stage K
comparison; the no-adapter shader expresses the same schedule with selector 0
for initial scatter, selector 2 for each intermediate gather/scatter, and
selector 1 for the final gather/integrate/scatter. Every substep resets the grid
frontier and re-bucketizes, matching stage2's fresh per-update grid buffers.

### Comparison policy and assertions

All cold words, enabled, lambda, liquid F, and non-sand logJp are bit-exact.
These fields never consume grid-derived state. Integer error-status words are
exactly `{0, UINT_MAX, UINT_MAX, 0, 0, 0}` after bucketization and every stage;
admitted row count is exactly 64 (homogeneous) or 36 (mixed). Numerical grid accumulators are not stored by
this compact fixture and are not claimed bit-exact. The original frozen grid
comparisons remain unchanged.

Except for structural-only sand K10 and branch-flip rows in sand K2/K5,
numerically compared grid-dependent float state requires finite values and
`abs(actual-reference) <= r * max(1, abs(reference))`, where
`r=max(8*FLT_EPSILON, 6*s*(K-1)/Q)`. At s=8 this gives K2: 4.8e-6,
K5: 1.92e-5, K10: 4.32e-5. Derivation of the transfer scale: a normalized PIC
gather carries one decoded quantum q=1/Q; D=4*B and the quadratic stencil has
|offset|≤1.5, giving 6q per D entry per gather; there are s*(K-1) gathers.
The reference-relative factor and unit floor are an explicit acceptance policy,
not a proof that nonlinear F/SVD/plasticity feedback remains within this bound.
Eight FP32 epsilons supply a local arithmetic-rounding floor. This policy was
chosen before native execution and must not be widened merely to make failures
pass; the execution owner must investigate any failure and obtain a bounded
policy/design decision if nonlinear amplification invalidates it.

Mismatch diagnostics identify case, substep, particle row, field name, actual
and expected words, delta and allowed cap. Every seed checks 1,536 exact words;
every final snapshot checks 1,024 hot and 512 compatibility-cold words (384
semantic cold words). Family leaves also require exactly one K2, K5 and K10 case.
The mixed leaf checks 864 seed words, 576 final hot words and 288 compatibility
cold words (216 semantic) per K, using the same per-row material-aware exactness
rules and derived numerical bounds. Decision B below adds a measured upper bound
only for homogeneous elastic K2/K5/K10 and sand K2/K5 constitutive state. Sand K10
checks structure and reports numerical divergence without asserting parity. An explicit family identifier prevents its leading
liquid row from accidentally selecting it into the homogeneous liquid leaf.

These frozen-oracle counts remain unchanged: 64 rows × 8 compatibility words =
512 words, of which 64 × 6 = 384 are semantic; the mixed fixture has 36 × 8 =
288 compatibility words and 36 × 6 = 216 semantic words. The direct no-adapter
conformance fixture retains its six-float, 24-byte cold ABI. Production cold rows
now add one transient u32 property index: 6 × 4 + 4 = 28 bytes, giving a
64 + 28 = 92-byte device pair while `GpuParticle` remains 22 × 4 = 88 bytes.

The current 4,103-edge, 13,044-row dual-owner device-accounting oracle is derived
separately from those historical measurements. Lookup bytes remain
8 × 684² + 16 × 513² = 7,953,552. Per-owner fixed bytes are
7,953,552 + 132 contour + 4 dummy terrain + 64 body + 32 shape + 40 dummy
material properties = 7,953,824. Empty dynamic storage is
2 × 92 + 16 + 4 + 12 + 16 + 4 + 16 + 48 + 24 = 324 bytes; loaded storage is
2 × 13,044 × 92 + 16 × 13,044 + 4 × 78,264 + 12 × 256 × 78,264 +
16 × 13,044 + 4 × 13,044 + 16 + 48 + 24 = 243,609,832 bytes. Thus the empty
peak is 2 × (7,953,824 + 324) = 15,908,296 bytes, the loaded peak is
2 × (7,953,824 + 243,609,832) = 503,127,312 bytes, and growth is
2 × (243,609,832 − 324) = 487,219,016 bytes. These are allocation formulas,
not new measured results; readback capacity is accounted separately.

### Decision B — solid constitutive-state measured upper bound under open investigation

Native execution exposed these first failures under the derived policy at substep 8:

| Case | Row | Field | Absolute divergence | Derived cap |
|---|---:|---|---:|---:|
| `sand-k2-s8` | 5 | `F.c0x` | 7.75e-6 | 4.8e-6 |
| `elastic-k2-s8` | 19 | `F.c1x` | 1.51e-5 | 8.27e-6 |
| `sand-k2-s8` | 5 | `D.c0x` | 7.13e-6 | 4.8e-6 |

The source comparison found no equation difference: the SVD, normalisation,
blend, projection and reconstruction are the same; only PPR's degenerate
`atan2` guard differs. This does not establish the cause of the divergence.

Decision B retains the derived policy above and adds
`kSolidConstitutiveOpenInvestigationMeasuredUpperBound = 5e-4` only for elastic K2/K5/K10 and sand K2/K5 solid
constitutive state: deformation gradient `F` (words 4–7), deformation-displacement
`D` (words 8–11), and sand `logJp` (word 13), using
`max(derived_relative, 5e-4) * max(1, abs(expected))`. Initial observed divergences were
`F` 7.75e-6 and 1.51e-5 and `D` 7.13e-6, against 4.8–8.3e-6 derived caps.
This is a **measured upper bound for the solid constitutive state under an open
investigation**, not a derived tolerance or a per-field envelope. Liquid, visco, mixed
(including its solid rows), and every other solid field retain their existing
derived caps; exact-field classification is unchanged. For numerically compared rows the budget still
rejects non-finite state and any divergence above `5e-4` relative to
`max(1, abs(expected))` across the covered K values. Exact fields still reject any bit
difference. Failure lines retain `upstream divergence` and identify
`policy=derived|known-divergence` and `owner=gradient|displacement|history`
for constitutive-state fields (`owner=other` for other fields). The budget
does not apply to any other field or family.

Earlier device evidence after the gradient-only budget established that
`upstream_elastic_k2_k5_k10` passes; the subsequent sand evidence is recorded below.
The leaves report per-K start/pass lines; an unreported pass is not evidence of
success. No new native run is claimed by this edit.

### Sand checkpoint classification and numerical coverage exclusions

The measured sand ladder at substep 8 is K2 `F.c0x` **7.75e-6**, K5
**1 flipped row (13 words, within the 2-row bound)**, and K10 with matching checkpoint predicates
`F.c0x` **2.58e-4**, followed by `D.c0x` **2.34e-3 against a 7.1e-4 cap**.
The observed divergences exceed the retained comparison budget; matching
checkpoint predicates do not establish equivalence of earlier GPU branch history.
**Sand K10 numerical comparison is therefore excluded.** Replacement of the sand
law is a hypothesis for B1, not a conclusion: a transcription defect or compiler
difference remains an alternative requiring the deferred differential trace.

Sand K10 is **structural-only**: the case executes, every hot/cold word must be
finite, inventory (mass, volume, material, colour, enabled) remains exact, and
the branch-flip count must stay within 2 rows. Existing exact hot fields also
remain exact. The measured maximum absolute hot-word divergence, with row and
field, is reported but not asserted. Each run explicitly logs
`upstream sand-k10 comparison=structural-only reason=ill-conditioned-return`.
Numerical bounds cover every row of the twelve liquid/elastic/visco/mixed
family/K cases and only non-excluded rows of sand K2/K5. Up to two rows per sand
case whose reconstructed checkpoint tuples differ are structural-only; the
recorded K5 run had one such row. All sand K10 rows are structural-only.
These are coverage exclusions, not numerical conformance. Exact seed/inventory
and execution-status contracts remain asserted for every case. No excluded row
is counted as numerically compared, and no numerical parity or physical validity
is claimed for it. The differential trace remains the first B1 task to establish
branch equivalence and localize amplification or possible transcription errors.

For each expected/actual `F`, reproduce `forkSvd`: with matrix entries
`(a,b,c,d) = (c0x,c0y,c1x,c1y)`, set `E=(a+d)/2`, `F0=(a-d)/2`,
`G=(b+c)/2`, `H=(b-c)/2`, `Q=sqrt(E²+H²)`, `R=sqrt(F0²+G²)`.
Clamp signed singular values `(Q+R,Q-R)` to `[0.1,10000]` as integration
does, then compute `eDiag=log(max(abs(Sigma),1e-6))`,
`traceE=eDiag.x+eDiag.y+logJp`, `frobenius=length(eDiag-traceE/2)`, and
`deltaGamma=frobenius+(elasticityRatio+1)*traceE*alpha`, where
`alpha=sqrt(2/3)*2*sin(frictionAngle/180*3.14159)/(3-sin(...))`.
The tuple is `(traceE>=0, deltaGamma>0, logJp==0)`; the last predicate
selects the constraint-stage singular-value clamp.

A tuple difference reports both tuples and derived values, labels every
bit-diverging hot word in that row `policy=branch-flip comparison=structural-only`,
reports its numerical delta, and counts rows
and words. **At most 2 branch-flip rows per case** are allowed; exceeding
that bound fails with `policy=branch-flip-count`. Sub-ulp perturbations
should change decisions only near a boundary, so a handful is expected;
many flips signal a systematic discrepancy requiring investigation. All
such rows must be finite, exact hot fields remain exact, and all cold
inventory words (mass, volume, material, colour and padding) remain exact.
The branch classification and its 2-row bound are unchanged; liquid/visco/mixed
policies are unchanged.

Post-projection checkpoint predicates do not recover earlier GPU branch history;
the differential trace must localize the amplification as well as the first
actual flip. The K5 classification and K10 failure above are supplied device
evidence. The sand K10 exclusion was exercised in the recorded post-A1 gate.
The pre-merge extension making flipped rows explicitly structural-only has not
been built or executed in this edit-only task.

### Phase A decision ledger

| Ruling | Rationale | Envelope / scope | Cost if wrong |
|---|---|---|---|
| Decision B — retain measured constitutive-state bound for remaining comparisons | Initial elastic/sand divergence exceeds derived caps; investigation remains open | `5e-4 * max(1, abs(expected))`, in addition to the derived cap, for elastic K2/K5/K10 and sand K2/K5 solid `F`, `D`, and sand `logJp`; at most 2 flipped rows per sand case are structural-only exclusions | A real defect below the budget can pass; differential trace remains B1's first task |
| Exclude sand K10 from numerical comparison; structural-only | K2 `F` 7.75e-6; K5 one flipped row; K10 `F` 2.58e-4 and `D` 2.34e-3 exceed the retained budget; earlier branch history and cause remain unknown | Execute, require finite words and exact inventory, bound branch flips to 2 rows, report maximum divergence without asserting it; explicit exclusion log every run | A sand K10 transcription error could go undetected until the differential trace or a gameplay symptom; post-A1 execution passed structural contracts only |
| A5 closed as a documented rejection for Phase A; retain current sand law | Candidate needs constitutive/constraint-policy choices; history halves at rest, eHat has trace -H, measured sand response depends on K, and checkpoint divergence remains unexplained | `sand_history_phase_a_characterization` pins the production committed pair and anisotropic projection at K2/K5/K10; reopen at B1 before sand property semantics freeze, or earlier on demonstrated supported-workload failure; gate artifact: `pbmpm-sand-history-derivation.md` | A wrong law stays shipped until B1. The leaf passed in the post-A1 gate, establishing regression coverage, not certification of correctness |

### Registration and execution handoff

New full tokens are:

```
app/matter_fork/upstream_liquid_k2_k5_k10
app/matter_fork/upstream_elastic_k2_k5_k10
app/matter_fork/upstream_sand_k2_k5_k10
app/matter_fork/upstream_visco_k2_k5_k10
app/matter_fork/upstream_mixed_k2_k5_k10
```

`lib/engine/tests/app/CMakeLists.txt` adds the header as a PRIVATE source and
changes `matter_fork_gpu` from the `app/matter_fork` prefix to an explicit list:
the six existing leaves plus these five, exactly once each. The list is 688
characters, below 1,024; the entry retains GPU resource lock, 300 s timeout,
GPU-validation label and `matter_fast` membership. The mixed extension adds one
token and one group recurse entry; the source census now matches 11 leaves to
11 unique tokens, each registered exactly once (previously 10 / 647 characters).

Expected work is five shader/device fixture initializations, 120 substeps, 360
bucket dispatches, and 680 fused dispatches over the fifteen cases. There are
per-stage waits/status readbacks, so kernel counts alone do not predict wall
time. The post-A1 fast gate passed in 551 s under GPU contention, as recorded
below; the 300 s shard and 420 s matter_fast ceilings remain acceptance gates.
The execution owner must validate the pre-merge branch-row revision and obtain
an exclusive fast-gate measurement. Windows/D3D12 remains a separate unrun
compatibility gate.

Edit-lane checks: format check passed on both touched C++ files; semantic style
reported zero blocking findings (two advisory B-CMP findings); scoped
`git diff --check` passed. An independent parser re-read the formatted header,
reconstructed all five seed arrays, and matched all fifteen seed/hot/cold
projections against source JSON. A source census matched all eleven
fork leaves to the eleven unique CMake tokens. These are artifact/style checks,
not GPU numerical-conformance evidence.

## A1 — landed: vendored generator and K2/K5/K10 conformance

`scripts/pbmpm/` (276 KB, 31 files) now holds the offline oracle tool: upstream sources vendored byte-identically at `61749b6c2d2631ed243f86882752ba43196f2de7` with per-file sha256 in `upstream/README.md`, the BSD-3-Clause notice, the compute-only `src/gpu.js` replacement marked as modified, `run.mjs`/`stage2.mjs` runners with no absolute host paths, pinned `webgpu@0.6.2` in `package.json`, and the invocation/determinism contract in the README. Generation is offline and never part of a build or gate.

Conformance: `App.MatterFork.Upstream.h` (199 KB) carries 15 upstream cases — four families plus one interacting mixed case (liquid 4×4 adjacent to sand 4×4 with an overlapping elastic patch, one-cell spacing against a 1.5-cell support radius) at K2/K5/K10 — consumed by five `app/matter_fork/upstream_*` leaves that run PPR's **no-adapter** path with upstream parameters (`dt = 1/120`, `Q = 1e7`, gravity 2.5, relaxations 1.5, viscosity .01, friction 30°, plasticity .9).

| Case group | K2 | K5 | K10 |
|---|---|---|---|
| liquid, visco, mixed | within the derived cap | within the derived cap | within the derived cap |
| elastic | bounded | bounded | bounded |
| sand | bounded on non-excluded rows | bounded except 1 structural-only flipped row | **structural only** |

Measured solid divergence: sand `F.c0x` 7.75e-6 at K2, 2.58e-4 at K10; sand `D.c0x` 2.34e-3 at K10; elastic `F.c1x` 1.51e-5 at K2. The 5e-4 measured bound covers numerically compared elastic and sand K2/K5 constitutive fields; flipped rows are structural-only exclusions. Sand K10 is excluded from numerical comparison because its measured divergence exceeds the retained budget. Matching final-checkpoint predicates do not establish earlier branch history. Each run logs `upstream sand-k10 comparison=structural-only reason=ill-conditioned-return`.

**Replacement remains a hypothesis for B1**, alongside repair of a transcription defect or investigation of compiler differences. The deferred differential trace must distinguish these explanations; the current evidence does not select one.

Gate note: after A1 the fast gate is green (24/24) but measured 551 s, with entries uniformly ~2× their exclusive reference (terrain 85→178 s, material 33→92 s) while another session held the GPU. The 420 s ceiling is therefore unchanged and unproven at this identity; an exclusive re-measurement is required before quoting it.

## Final pre-merge acceptance (identity `3095e1bb`)

Identity: HEAD `3095e1bbbaefe6a68121ae4da52e50e145a284a7`; shader set `4154830028c2a0a6`; `engine.tests.app` `212426a5e0f8ca6d`; `game.tests` `2acf41eee25c2955`. GPU 20 % before / 18 % after, clock 1245 → 1515 MHz (no competing compute).

| Run | Result | Wall |
|---|---|---:|
| `matter_fast` #1 | 24/24 passed | 336 s |
| `matter_fast` #2 | 24/24 passed | 333 s |
| `matter_fast` #3 | 24/24 passed | 338 s |

Three consecutive runs, all inside the 420 s ceiling, with the new conformance leaves, the sand-history characterization leaf and the corrected structural-only exemptions included. `matter_long` evidence is reused from `5355f3ee` (2/2, 2378 s) because no production source changed between that identity and this one — only tests, docs and the vendored tool.

**Still open at merge time**

- **Windows/D3D12 unrun.** Required by the plan as a compatibility gate; Linux evidence cannot satisfy it.
- **Merge surface.** Three paths overlap with the current `particles` tip: `game/colony/codemap.md`, `lib/engine/app/renderer/App.Renderer.GridPass.cpp` and `.cppm`. Main has advanced past this branch's base with renderer ownership/reconstruction work, so integration and affected rendering tests are still required.

## B1 increment 1 — historical per-substance plumbing handoff

This subsection records the preceding increment. Its v8 layouts and placeholder
mapping are superseded by the increment-2 decisions and authored coverage at the
top of this document.

Every substance now carries an independently editable constitutive-family/coefficient row instead of inheriting one global set. The default coefficient rows currently carry legacy values; independent editability is not distinct per-substance tuning. Reference density comes from each catalog entry. Only explicitly documented element-specific entries are tuned; this table does not claim further tuning.

| Element | Family | Note |
|---|---|---|
| water, brine, oil/crude, magma | liquid | independently editable rows; legacy viscosity and catalog reference density |
| sand, sandstone, basalt, coal, copper/iron/uranium ores, wolframite, autunite, azurite | sand | independently editable rows; legacy friction and catalog reference density |
| clay / wet sand, snow, pebbles / gravel | sand | **explicit placeholders** until the cohesive and compacting laws land |

Layout: the semantic record stays 88 bytes; the device cold row grows 24 → 28 bytes for the property index (`kDeviceRowBytes = 92`). `PBMPM_ENGINE_PARTICLE_ROWS` explicitly selects the 28-byte engine layout in both the solver and sparse maintenance. Conformance explicitly selects the 24-byte ABI and global profile; its pinned fixtures and arithmetic are unchanged. An unspecified or conflicting selection is now a compile-time error. The earlier stride mismatch made sparse maintenance read the property sentinel as a NaN mass: sparse admission reports **flag 8** for malformed particle fields and can suppress the fused dispatch. The earlier attribution to a “flag-512 property rejection” was wrong. Flag 512 belongs to the fused solver's separate property-table guard (field 0: out-of-range storage; field 1: empty/family-mismatched row).

Checkpoint envelope **v8** stores the material-table policy, a canonical table identity and the complete substance rows; per-particle indices are reconstructed, never persisted. v6 migrates to K2 and v7 to its saved schedule, both with legacy-valued rows; a policy or identity mismatch returns `protocol_not_supported`.

**Authored coverage pending execution:** `app/matter_gpu/substance_rows_preserve_defaults_and_reject_invalid_replacements` asserts default-row equivalence and invalid-table admission/epoch isolation. Its previous semantic-row assertions did **not** establish append index preservation. The extended coverage now reads the retained particle's actual cold index, grows a table with differentiated same-family viscosity rows, checks index/coefficient resolution after cancellation and publication, and compares execution with a replacement control whose indices are rebuilt against a permuted table. `same_family_substances_differ_and_unused_rows_do_not_dispatch` covers differentiated shear response and unused-row dispatch/result invariance. `device_property_guard_reports_out_of_range_and_family_mismatch` executes the production property guard against corrupted device table inputs and asserts flag 512 plus index/field/particle witnesses, separately from host admission. `reflected_scalar_protocol_and_source_helpers` now checks the sparse maintenance cold stride/property offset as well as the solver ABI. The checkpoint leaves `current_round_trip_preserves_all_words_and_rebinds_only_world` and `saved_schedule_migrates_v6_and_rejects_invalid_counts_without_mutating_live_world` cover nondefault v8 coefficient words, legacy v6/v7 migration, a pinned independently computed two-row table hash and row-order invariance. Candidate-restore isolation means a separate failed restore leaves the live driver's capture unchanged; GPU admission/epoch assertions remain in the GPU coverage. No build, test, frozen-fixture rerun or backend compatibility result is claimed for these review edits.

At that handoff the cohesive/compacting laws and pebble coefficients were still
ahead. Increment 2 now authors those laws and the elastic diagnostic; execution
and the recovery determination remain pending. The authoritative decisions and
accepted risks are in `docs/pbmpm-material-laws-proposal.md`.
