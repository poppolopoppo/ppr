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

- A1–A6 of the plan are not started: reference provenance, arithmetic-domain guards, generalized scheduling, operating-point decision, sand-history derivation, and the final Phase A acceptance run.
- D3D12/Windows verification is unavailable on this host and remains an unrun gate.
- Only the Debug/ASan/validation configuration has been measured; Release timing and cost evidence is pending.
