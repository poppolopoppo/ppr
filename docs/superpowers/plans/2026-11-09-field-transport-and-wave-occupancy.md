# Field Transport and Wave Occupancy Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Cut the world-light field's GPU cost and make the terrain legible by replacing the per-evaluation 192-load cascade gather with a per-cell radiance atlas plus update-time coarse→fine collapse, then raise wave occupancy on Turing with LDS staging, wave intrinsics and VGPR reduction.

**Architecture:** A new `WorldLightRadianceAtlas` owns one radiance value per native cell (written by a direct-lighting compute pass with a temporal EMA) — this is the oni_v22 `map.radiance` / `mapRadianceSample` shape. The cascade trace stops gathering the previous field at every opaque hit and instead samples that one atlas value; coarse levels are collapsed into the finer ones **at update time** (`Cascade::computeLevelAndMergeParent`) so shading reads the finest level only. The terrain fragment shader then samples one level instead of six, and reads the atlas for the irradiance that lights dug borders. Occupancy is treated as a first-class budget: the field compute and terrain pixel shaders get explicit registers-per-thread targets on SM 7.5, met with shared-memory staging, wave-uniform control flow and reduced live ranges.

**Tech Stack:** C++23/C++20 modules, Slang (→ SPIR-V, Vulkan via Slang-RHI), engine RHI/Shader services, CMake presets, PPR unit-test framework, Nsight Graphics GPU Trace, Nsight Systems GPU Metrics, perf.

**Spec:** `docs/superpowers/specs/2026-11-07-world-light-field-design.md` (extends it; the accepted deltas are recorded under "Accepted design delta" below).

## Accepted design delta (2026-11-09)

Validated by the user. These supersede the corresponding parts of the 2026-11-07 design.

- **One gather, not six.** Shading and the field's own bounce read a single finest-level value. Coarse cascade levels are merged into the finer level during `update()`, never composited per fragment.
- **Per-cell radiance atlas.** One radiance sample per native cell, written by a dedicated direct-lighting pass with temporal EMA. It is both the cascade's surface radiance source and the terrain's legibility term.
- **Bounded legibility.** Interior terrain must be readable around dug borders; the atlas provides it. This is an authored look change and is accepted.
- **Turing occupancy is a budget, not an afterthought.** SM 7.5: 64 warps/SM, 65,536 32-bit registers/SM, 64 KB unified L1/shared (48 KB default per block, 64 KB opt-in), 16 blocks/SM, fixed subgroup size 32. Occupancy from registers is `warps = min(64, floor(65536 / (regs_per_thread * 32)))`.
- **SGPR/VGPR note.** SGPR/VGPR are AMD terms. On NVIDIA the analogue is uniform registers plus the uniform datapath, which is compiler-scheduled and cannot be pinned from SPIR-V. We steer it by keeping values wave-uniform, reading per-wave constants from constant buffers, and eliminating per-lane recomputation; we verify with `SHI_REGISTERS` and the register-limited stall counters.
- **Wave intrinsics are allowed** via Slang `WaveActive*` / `WavePrefix*` / `WaveReadLaneAt` / `WaveMatch` (SPIR-V `GroupNonUniform*`). Precedent in this codebase: `quadSwapX/Y/Diagonal` already lower to SPIR-V quad-group ops.
- **CPU descriptor work is out of scope.** GPU is 99.2 % active; it is deferred until a GPU-side change makes CPU the limiter.

## Global Constraints

- Map sizes: 1024² baseline, 2048² maximum, 4096² benchmark only, never a delivery gate.
- Level 0 spacing stays 2 native cells. Do not silently coarsen it.
- One 128² chunk quad per chunk; never per-cell geometry.
- Camera-independent field; no reprojection, no camera-dependent reset.
- 60 Hz target. Field update budget: **≤ 6 ms at 1024² and 2048²**. If over, lengthen the refresh cycle; never coarsen spacing or drop dirty work.
- Register budgets from Task 1: **terrain PS ≤ 40 regs/thread (≥ 51 warps/SM)**; **field CS ≤ 48 regs/thread with 128-thread blocks (≥ 42 warps/SM)**. At `[numthreads(64,1,1)]` the 16-blocks/SM limit caps the field at 32 warps regardless of registers, so threadgroup shape and register budget are one budget. Stretch target 32 regs (64 warps).
- Frame must not regress: no task may raise steady-state frame time; measure, do not assume.
- No `-ffast-math`, `-ffp-contract=fast`, or FTZ in shader math that affects simulation or lighting determinism.
- Every task ends with build + focused tests + a measurement delta + a commit.
- Visual evidence comes from the engine capture path (Task 1a). Do not drive the OS window with external screenshot tools; that approach produced no reliable evidence and cost more time than it saved.
- The probe-alignment verdict from the in-flight debug-view lane is a hard prerequisite for Task 1; if it finds a producer bug, fix it before any transport change.

## Review Focus

- A dug border must be visibly lit within one refresh cycle, and must not bleed light through solid rock into unrelated tunnels.
- The atlas EMA must stay finite and non-negative for dark, high-albedo and coloured-bounce scenes; no per-frame readback.
- Collapsing coarse→fine must not double-count sky or drop the terminal sky contribution at the world edge.
- Register/occupancy budgets must be met on the LDS/wave-intrinsic paths without changing visual output beyond the accepted legibility bleed.
- Wave-uniform early-outs must never be applied when lane results differ (divergence correctness), and subgroup ops must be gated on device support.

---

### Task 1: Baseline gate — probe correctness and occupancy budget

**Files:**
- Read: `lib/engine/app/renderer/App.Renderer.WorldLightField.cpp` (`evaluate()`, `view()`), `assets/shaders/world_light_sample.slang`
- Test: `lib/engine/tests/app/App.WorldLightField.Tests.cpp`
- Record: `docs/superpowers/plans/2026-11-09-field-transport-baseline.md` (tracked; commit it)

**Interfaces:**
- Produces: the numbers every later task is judged against — registers/thread and warps/SM per shader, per-pass ms, and the numeric probe scan.

- [ ] **Step 1: Record the numeric probe scan.** The tree's `evaluate()` seam returns one composite/diffuse/specular triple per call and has **no per-level output**, so the scan is a set of positions sampled one at a time. Record the validated sites (lamp, adjacent air, open room, sealed cavity, solid adjacent to air, out-of-world) with all three terms each, plus the real-game air-cell sample. If any site shows a displaced peak or flat zero, stop and fix the producer before continuing.
- [ ] **Step 2: Record per-pass GPU time** for `WorldLight.FieldUpdate`, `Grid.Terrain`, `Grid.Translucent`, `Particles.Draw`, `PBMPM.Tick` from a fresh Nsight Graphics capture at 1024² steady state.
- [ ] **Step 3: Record registers/thread and warps/SM** for the field compute and terrain pixel shaders from a Graphics Capture with `Collect Shader Pipelines` (`SHI_REGISTERS`), plus `PS/CS Warp Can't Launch – Register Limited` and `Unallocated Warps in Active SMs` from Nsight Systems GPU Metrics. State which shader misses which budget.
- [ ] **Step 4: Commit the baseline doc.**

```bash
git add docs/superpowers/plans/2026-11-09-field-transport-baseline.md
git commit -m "docs: record world light field baseline and occupancy budgets"
```

---

### Task 1a: Engine-native frame capture to PNG

Every visual gate in this plan — and the staging fix's own evidence — needs a frame image, and agents keep burning time driving the OS window with external screenshot tools. Capture must live in the engine.

**Files:**
- Modify: `lib/engine/app/App.Application.Editor.cpp` (the surface capture request already exists as `m_capture_final_frame` / `m_surface_capture_sequence`)
- Modify: `lib/engine/app/renderer/App.Renderer.cpp` (`prepareSurfaceReadback_` / `completeSurfaceReadback_` already produce `SurfaceFramePixels`)
- Modify: `game/Game.Config.cppm`, `game/Game.Config.cpp` (a one-shot `--capture-frame <path>` setting)
- Test: the existing surface-readback/frame tests in `lib/engine/tests/app/`

**Interfaces:**
- Produces: `std::error_code writeSurfacePng(const SurfaceFramePixels &, const std::filesystem::path &)`; a one-shot trigger that captures the next completed interactive frame to that path and logs it once. Zero cost when unset.
- Consumes: `Renderer::prepareSurfaceReadback_` / `completeSurfaceReadback_`, `SurfaceFramePixels` (RGBA/BGRA8).

- [ ] **Step 1: Write the failing test** `surface_pixels_round_trip_through_png`: build a small known `SurfaceFramePixels` pattern, write it, decode it back with the existing image service, assert pixel equality.
- [ ] **Step 2: Run it to verify it fails.**
- [ ] **Step 3: Implement the writer.** Reuse an existing encoder from the image service if one exists; otherwise check `vcpkg` for libpng/zlib and prefer a minimal deflate PNG writer over a new dependency.
- [ ] **Step 4: Wire the one-shot trigger** to the existing capture request; write the file and log its path exactly once.
- [ ] **Step 5: Validate**: run `app.game --map-size=1024 --auto-exit=30 --capture-frame /tmp/opencode/frame.png`, assert the file exists, decodes, and has the surface dimensions; assert no frame-time change when the flag is unset.
- [ ] **Step 6: Commit** `app: capture the presented frame to a PNG`.

---

### Task 1b: Simplify the world-light debug view

The visual mode is correctly sized; two pieces are not. Removal is approved by the user.

**Files:**
- Modify: `lib/engine/app/App.Application.Editor.cpp` (delete `findLitSamplePosition_`, `evaluateWorldLightProbe_`, the unconditional firing block, and the probe state fields)
- Modify: `game/Game.Config.cppm` (drop the `irradiance` arm of the debug enum)
- Modify: `assets/shaders/grid_tiles.slang` (drop the `2u` arm; read `g_world_light_field.levels[0].spacing` instead of the hardcoded `2.0`; re-derive the false-colour scale)
- Modify: `lib/engine/tests/app/App.WorldLightField.Tests.cpp` (move the numeric evidence into the existing fenced readback test; drop assertions for the removed enum value)
- Test: `lib/engine/tests/app/App.WorldLightField.Tests.cpp`

**Interfaces:**
- Produces: `EWorldLightDebugView { off, probes }` only, with no runtime readback; the debug path costs nothing when `off`. The numeric evidence lives in the test harness's fenced readback, not in the application.

- [ ] **Step 1: Write the failing tests** `probe_sample_at_lamp_is_non_zero` (fenced readback via `fixture.read`/`submit`/`FrameFence`; assert the lamp composite is non-zero and finite) and `debug_view_has_only_off_and_probes` (config enum).
- [ ] **Step 2: Run them to verify they fail.**
- [ ] **Step 3: Delete the runtime readback machinery and the `irradiance` arm**; move the numeric assertion into the test; fix the `kChunkEdge` ceil/floor duplication if it is still present in the removed code.
- [ ] **Step 4: Run the focused tests**; confirm the application issues no per-frame GPU stalls with the debug view `off` (grep the run log; frame time unchanged).
- [ ] **Step 5: Commit** `lighting: simplify the world light debug view`.

---

### Task 2: Per-cell radiance atlas

**Files:**
- Create: `lib/engine/app/renderer/App.Renderer.WorldLightRadianceAtlas.cppm`, `.cpp`
- Modify: `lib/engine/app/renderer/App.Renderer.WorldLightTypes.cppm` (plan/settings if the atlas needs a resolution)
- Create: `assets/shaders/world_light_radiance_atlas.slang`
- Test: `lib/engine/tests/app/App.WorldLightField.Tests.cpp`

**Interfaces:**
- Produces: `class WorldLightRadianceAtlas` with `initialize(IRhiService&, IShaderService&, const WorldLightPlan&, const path&)`, `std::error_code encode(ICommandEncoder&, const WorldLightSourceGpu&)`, `rhi::IBuffer *cells() const`, `void setTemporalRetention(float)`; storage is one packed RGB9E5 `u32` per native cell, row-major `width × height` (oni `alias PackedRadiance = RGB9E5`).

- [ ] **Step 1: Write the failing test** `atlas_seeds_emissive_cells_and_retains_history`: seed a 64×64 source with one emissive cell, run `encode()` twice with retention 0.5, read back `cells()`; assert the emissive cell is non-zero after the first pass and that the value decays toward the new sample rather than snapping.
- [ ] **Step 2: Run it to verify it fails** (`ctest -R "^engine.tests.app.world_light_field$"`), expected failure: atlas not found / zero.
- [ ] **Step 3: Implement the atlas and its compute shader.** One thread per cell; radiance = material emission + albedo × previous atlas value (one bounce, oni `mapDirectLighting`); temporal EMA `mix(new, prev, exp(-k·dt))`; pack with the existing `RGB9E5` helpers from `grid_liquid_kernel`/`world_light_field.slang` (reuse, do not duplicate).
- [ ] **Step 4: Run the test to verify it passes.**
- [ ] **Step 5: Commit** `lighting: add per-cell radiance atlas`.

---

### Task 3: Trace samples the atlas at the opaque hit

**Files:**
- Modify: `assets/shaders/world_light_trace.slang` (opaque hit branch, ~line 132)
- Modify: `assets/shaders/world_light_sample.slang` (remove `worldLightDiffusePrev` from the trace path; keep it if a test still uses it)
- Modify: `lib/engine/app/renderer/App.Renderer.WorldLightField.cpp` (bind the atlas on the update root)
- Test: `lib/engine/tests/app/App.WorldLightField.Tests.cpp`

**Interfaces:**
- Consumes: `WorldLightRadianceAtlas::cells()`, on three preconditions: the atlas `encode()` is recorded earlier in the same command buffer; the buffer is transitioned with `encoder.setBufferState(atlas.cells(), rhi::ResourceState::ShaderResource)`; and the RGB9E5 decode comes from the shared shader include the atlas also uses, never a local re-declaration.
- Produces: `float3 worldLightAtlasRadiance(float2 world)` in `world_light_sample.slang` — one `StructuredBuffer<u32>` load plus the shared decode.

- [ ] **Step 1: Write the failing test** `field_bounce_reads_atlas_not_previous_field`: two runs with identical sources but different previous-field contents must produce the same bounce term when the atlas is fixed; and a reflective-wall case must remain non-zero (reuse `field_feedback_bounces_off_reflective_wall`).
- [ ] **Step 2: Run it to verify it fails.**
- [ ] **Step 3: Replace the hit-time gather.** `radiance += transmittance * (emission + albedo * worldLightAtlasRadiance(p))`. Delete the per-hit 8×6×4 gather from the trace.
- [ ] **Step 4: Run the focused tests; then measure** `WorldLight.FieldUpdate` against the Task 1 baseline. Expected: a material drop; if it does not drop, stop and report rather than layering more changes.
- [ ] **Step 5: Commit** `lighting: trace bounce from the radiance atlas`.

---

### Task 4: Collapse coarse→fine at update time

**Files:**
- Modify: `lib/engine/app/renderer/App.Renderer.WorldLightField.cpp` (`update()`, `view()`)
- Modify: `assets/shaders/world_light_trace.slang` (`computeLevelAndMergeParent` equivalent)
- Modify: `assets/shaders/world_light_sample.slang` (`worldLightDiffuse`/`worldLightSpecular` read one level)
- Test: existing field tests + a new one

**Interfaces:**
- Consumes: per-level radiance buffers (unchanged layout).
- Produces: `WorldLightFieldView` guarantees `m_radiance` holds the **finest, fully merged** field; `m_previous_radiance` remains for feedback parity tests; the level array is still exposed for debug views.

- [ ] **Step 1: Write the failing test** `merged_field_preserves_terminal_sky_at_world_edge` and `merged_field_matches_per_level_composite_within_tolerance`: compare a CPU oracle composite of all levels against the merged finest level at several positions.
- [ ] **Step 2: Run them to verify they fail.**
- [ ] **Step 3: Implement merge-at-update.** After tracing level `l`, merge it into `l-1` with `L = L_near + T_near·L_far` (oni `cascadeMergeInterval`), coarse→fine, once per update; shade from level 0.
- [ ] **Step 4: Run the field tests; measure** `FieldUpdate` and `Grid.Terrain` against baseline.
- [ ] **Step 5: Commit** `lighting: collapse cascade levels at update time`.

---

### Task 3b: Atlas input — per-cell incident radiance from the merged field

The atlas currently stores `emission + albedo * previous_atlas`, a self-referential quantity: it double-counts emission (the trace also adds emission inline), its fixed point diverges as albedo approaches 1, and it is exactly zero for every non-emissive cell, so no light is ever carried laterally and Task 6's legibility term would be dead.

**Files:**
- Modify: `assets/shaders/world_light_radiance_atlas.slang`
- Modify: `lib/engine/app/renderer/App.Renderer.WorldLightRadianceAtlas.cpp` (bind the merged previous field and field constants)
- Modify: `lib/engine/tests/app/App.WorldLightField.Tests.cpp`
- Test: same

**Interfaces:**
- Produces: `atlas[cell] = pack(lerp(L_i(cellCentre), previous, retentionWeight))` where `L_i = worldLightDiffuseFrom(mergedPrevField, cellCentre, +Z)`; the vacuum fast path is preserved. **The atlas stores incident radiance only — never emission.** Both consumers are already written for this: the trace adds `emission + albedo * atlas` and Task 6 adds `kAtlasAmbient * albedo * atlas`.
- Consumes: the merged field produced by Task 4; `g_world_light_radiance_prev` must hold a complete merged field.

**Gate:** Task 4 landed. Gathering the pre-merge six-level field per cell is ~192 loads x ~1M cells (~20 ms) and would destroy the Task 3 win; the merged field reduces it to a handful of loads per cell.

- [ ] **Step 1: Make the bounce test honest again.** Demote the directly-seeded `field_feedback_bounces_off_reflective_wall` to a wiring test named `atlas_seed_drives_trace_bounce` with a comment saying it seeds the atlas, and add an end-to-end `wall_reflects_lamp_through_atlas` that drives a real atlas encode from a lamp.
- [ ] **Step 2: Add the dug-border contract to the air-probe test.** A cell just outside a one-cell opening in a walled room has `diffuse > 0` and finite; deep interior rock and a sealed-cavity centre stay exactly zero; and a cell with `E > 0, rho > 0` satisfies `composite ≈ E + rho * L_i` where `L_i` is read back from `atlas.cells()` directly (RGB9E5 tolerance).
- [ ] **Step 3: Pin the shipped GPU fold.** Add `gpu_fold_matches_cpu_merge_with_partial_transmittance`: seed the previous buffer with a non-trivial per-level hierarchy (anisotropic radiance plus partial transmittance), run one update with retention 0 and accumulation 1 over a vacuum probe, and assert the read-back level 0 against the CPU fold. Task 4's tests cover the CPU fold formula and the single-level sampler but never execute `worldLightParentSampleFrom` + the `accumulateSample` fold, so a doubled sky, a wrong transmittance product or a wrong direction remap could currently pass.
- [ ] **Step 4: Run the four tests red.**
- [ ] **Step 5: Implement the producer.** Bind the merged previous field and constants, compute `L_i` at the cell centre, store incident only, keep the vacuum fast path, delete the emission term.
- [ ] **Step 6: Run the tests green; measure** the atlas pass and `FieldUpdate`. The atlas pass must stay well under the field update.
- [ ] **Step 7: Commit** `lighting: feed the atlas from the merged field`.

---

### Task 5: Circular-harmonics fine level (conditional)

**Files:**
- Modify: `assets/shaders/world_light_field.slang`, `world_light_trace.slang`, `world_light_sample.slang`
- Modify: `lib/engine/app/renderer/App.Renderer.WorldLightField.cppm` (sample layout/bytes)
- Test: `App.WorldLightField.Tests.cpp`

**Gate:** implement only if Task 4's field update is still > 6 ms at 1024², or the atlas working set still misses the L2 budget. Otherwise skip and record why in `baseline.md`.

- **Skipped 2026-11-09:** the field update is 1.95 ms against the 6 ms gate. The L2/working-set lever is Task 9's LDS staging; CH would shrink only level 0 (~12% of bytes) and would not touch the coarse levels that dominate the working set.

**Interfaces:**
- Produces: `CircularHarmonics<4>` (7 `float3` coefficients, oni `Packing::Radiance9`) per level-0 probe; `worldLightDiffuse` becomes `Σ A_m · coeff_m` (oni `diffuseIrradiance`).

- [ ] **Step 1: Write the failing test** `ch_evaluation_matches_directional_composite_for_smooth_fields` (compare against the Task 1b asserted site set within tolerance).
- [ ] **Step 2: Run it to verify it fails.**
- [ ] **Step 3: Implement projection at trace time and evaluation at sample time**, RGB9E5-packed as in `Packing::packRadiance9`.
- [ ] **Step 4: Run tests; measure** field update, L2 hit rate, and radiance bytes.
- [ ] **Step 5: Commit** `lighting: store the fine cascade in circular harmonics`.

---

### Task 6: Terrain legibility from the atlas

**Files:**
- Modify: `assets/shaders/grid_tiles.slang` (`shadeWorldLightResponse` / `shadeWorldLightSurface`)
- Modify: `game/Game.Config.cppm` (bounded bleed strength, default matches the accepted look)
- Test: config default + a GPU shading test if the harness supports it

**Interfaces:**
- Consumes: `worldLightAtlasRadiance` (incident radiance, per Task 3b).
- Produces: `+ kAtlasAmbient · albedo · atlasRadiance` in the response, with the existing self-emission added exactly once.

**Gate:** Task 3b landed. Before it, the atlas carries no incident light and this term is dead.

- [ ] **Step 1: Write the failing test** `world_light_atlas_ambient_defaults_to_visible_borders` (config default) and, where the harness allows, a shading assertion that a cell adjacent to lit air is brighter than a deep interior cell.
- [ ] **Step 2: Run it to verify it fails.**
- [ ] **Step 3: Implement the ambient term** and the config knob; keep the diffuse/specular terms unchanged.
- [ ] **Step 4: Run tests; capture** a controlled screenshot set (dark emitter, daylight, dug border) and confirm borders are readable without flooding rock.
- [ ] **Step 5: Commit** `lighting: light terrain borders from the radiance atlas`.

---

### Task 7: Correct per-pixel terrain A/B

**Files:**
- Modify: `assets/shaders/grid_tiles.slang` (add a per-pixel counterpart of `fragmentGridReactive`)
- Modify: `lib/engine/app/renderer/App.Renderer.GridPass.cpp:3422` and the program selection at `:2034`
- Test: `lib/engine/tests/app/App.GridPass.Tests.cpp`

**Interfaces:**
- Produces: `fragmentGridReactivePixel` — identical outputs to `fragmentGridReactive`, no `SV_SampleIndex`, sampled per pixel while the target stays 4× MSAA.

- [ ] **Step 1: Restore a pass-level terrain GPU marker.** Add `pushDebugGroup("Grid.Terrain", ...)` + a deferred `popDebugGroup()` in `GridPass::renderGpuIndirect`, and the equivalent for `Grid.Translucent`. Pass-level markers must **not** be mixed with the encoder-level field markers (`WorldLight.FieldUpdate`/`FieldTrace`/`FieldCopy`) in the same command buffer: that combination previously corrupted the compute span boundaries in the captured trace. Verify the marker appears in `D3DPERF_EVENTS.xls` and that the field spans remain sane.
- [ ] **Step 2: Write the failing test** `grid_reactive_pixel_variant_shares_the_reactive_outputs`.
- [ ] **Step 3: Run it to verify it fails.**
- [ ] **Step 4: Implement the variant** and select it behind a temporary switch.
- [ ] **Step 5: Measure both variants** with Nsight Graphics (`Grid.Terrain` ms, register-limited PS stall, warps/SM, instructions). Normalize against a pass unchanged by this work to cancel machine drift. Record the result in the baseline doc.
- [ ] **Step 6: Commit** `renderer: add per-pixel reactive terrain variant for measurement` (keep the switch defaulted to the current path until Task 8 decides).

---

### Task 8: Terrain pixel-shader register budget (≤ 40 regs)

**Files:**
- Modify: `assets/shaders/grid_tiles.slang`, `assets/shaders/world_light_sample.slang`
- Test: Task 7's variant test + screenshot parity

**Interfaces:**
- Consumes: Task 7's measurement and Task 1's budget.
- Produces: terrain PS at ≤ 40 regs/thread (≥ 51 warps/SM), or the per-pixel variant enabled if Task 7 shows it is strictly better.

**Gate:** Tasks 3 and 4 landed; the inlined six-level gather is gone from `worldLightDiffuse`. Starting this while shading still composites six levels measures the wrong shader.

- [ ] **Step 1: Add the failing acceptance check** — record `SHI_REGISTERS` and the register-limited stall in `baseline.md` and assert the budget in the task report (a measurement gate, not a unit test).
- [ ] **Step 2: Reduce live ranges.** Order computation so gradients, decoration and albedo die before the lighting term; use `half` for colour-only intermediates; hoist wave-uniform constants; avoid `float4` temporaries that span the whole body.
- [ ] **Step 3: Re-capture** and compare against Task 1: registers/thread, warps/SM, reg-limited stall, `Grid.Terrain` ms, and pixel parity on the Task 6 screenshots.
- [ ] **Step 4: Commit** `renderer: reduce terrain pixel shader register pressure`.

---

### Task 9: Field compute — read-only buffers, 128-thread blocks, LDS staging

A tile covers a ray only when `tile >= probe span + 2 * far`, which only level 0 satisfies; levels >= 2 never do. Stage level 0 only.

**Files:**
- Modify: `assets/shaders/world_light_trace.slang`
- Modify: `lib/engine/app/renderer/App.Renderer.WorldLightField.cpp` (`kNumthreads`, dispatch count, buffer states)
- Test: `lib/engine/tests/app/App.WorldLightField.Tests.cpp`

**Interfaces:**
- Consumes: `g_world_light_ids`, `g_world_light_overlay`.
- Produces: `[numthreads(128,1,1)]` with the dispatch count halved; ids/overlay bound as read-only `StructuredBuffer` (`ld.global.nc`); a `groupshared` tile of one packed `u32` per cell (`id | opaque << 16 | hasOverlay << 17`), `110x110` at 1024² (~48.4 KB), replacing the two per-step global reads, with a global fallback outside the tile.

- [ ] **Step 1: Read-only binding first.** Declare ids/overlay as read-only `StructuredBuffer` in the trace and bind them `ShaderResource`. Write the failing test `read_only_source_trace_matches_rw_source`, make it pass, and measure.
- [ ] **Step 2: Write the failing test** `lds_staged_trace_matches_global_trace` — same probe/direction set, identical radiance within half-precision tolerance.
- [ ] **Step 3: Implement the level-0 tile**: cooperative strided load, one `GroupMemoryBarrierWithGroupSync()`, global fallback on tile exit; keep the global path behind a compile-time switch for A/B.
- [ ] **Step 4: Raise the threadgroup to 128** and halve the dispatch count; verify the block limit no longer caps occupancy.
- [ ] **Step 5: Run tests; measure** `l1tex__data_pipe_lsu_wavefronts_mem_lg`, L2 hit rate, `FieldUpdate` ms, registers/thread, warps/SM.
- [ ] **Step 6: Commit** `lighting: stage the dense source in shared memory`.

---

### Task 10: Field compute — the two wave intrinsics that pay

Only two are worth it for independent-direction rays. `[WaveSize(32)]` is a no-op on the Vulkan/SPIR-V path (Turing is fixed at 32) and must not be added. The wave-intrinsic precedent in this repo is `terrainContactWave` (`pbmpm/environment.slang`), not `quadSwap*`.

**Files:**
- Modify: `assets/shaders/world_light_trace.slang`
- Modify: `lib/engine/app/renderer/App.Renderer.WorldLightField.cpp` (capability probe; no subgroup-properties plumbing exists yet)

**Interfaces:**
- Produces: `WaveActiveBallot` whole-wave skip when no lane in the wave needs a trace; `WaveActiveAllEqual` + `WaveReadLaneAt` same-cell hoist for level >= 1 where neighbouring probes share the DDA cell. Skips wave-cooperative DDA, `WavePrefixSum` and `WaveMatch` (wrong shape for independent directions).
- Consumes: a capability check built on `IRhiService::hasFeature` (there is no existing `VkPhysicalDeviceSubgroupProperties` plumbing); the scalar path stays when the check fails.

- [ ] **Step 1: Write the failing tests** `wave_fast_path_disabled_without_subgroup_support` and `wave_uniform_hoist_preserves_per_lane_results` (compare against the scalar path on a deliberately divergent probe set).
- [ ] **Step 2: Run them to verify they fail.**
- [ ] **Step 3: Implement both paths** behind the capability gate; never take a uniform hoist when `WaveActiveAllEqual` is false.
- [ ] **Step 4: Run tests; measure** `FieldUpdate` ms, warps/SM, `Unallocated Warps`, `SM Issue Active`, L2 hit.
- [ ] **Step 5: Commit** `lighting: add wave-coherent field trace paths`.

---

### Task 11: Close-out

**Files:**
- Modify: `docs/superpowers/plans/2026-11-09-field-transport-baseline.md` (final numbers)
- Modify: `docs/superpowers/specs/2026-11-07-world-light-field-design.md` (record the accepted delta)
- Delete: any superseded gather/merge paths left behind

- [ ] **Step 1: Full validation.** Build `engine.tests.app game.tests app.game`; run `app.game.field_run`, `engine.tests.app.world_light_field`, `game.tests`, and the format/semantic-style gates.
- [ ] **Step 2: Budget check** at 1024² and 2048²: field update ≤ 6 ms, frame time not regressed, registers/thread within budget, no `+222 %`-style surprises.
- [ ] **Step 3: Controlled captures** (dark emitter, daylight, dug border, translucent/refraction) attached to the report.
- [ ] **Step 4: Delete dead code** — the per-hit gather, any level-compositing loop in shading, and the measurement-only switches.
- [ ] **Step 5: Commit** `lighting: close out field transport and occupancy work`.
