# World Light Field Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the current receiver-window world-radiance machine with one persistent, camera-independent 4-direction radiance-cascade hierarchy, a dense dirty-upload source, triangle/indirect terrain shading, and a histogram/SPD post chain, then delete the superseded code.

**Architecture:** A `WorldLightSourceGpu` owns dense per-cell ids, one global material palette, a dense Matter overlay and an occupancy pyramid. A `WorldLightField` owns one radiance-cascade hierarchy (level 0 = 2-cell spacing / 4 directions; level `l` = `2^(l+1)` spacing / `4^(l+1)` directions), accumulates per-direction samples temporally without reprojection, and composites levels near-to-far in the fragment shader. `GridPass` keeps its 128² chunk-quad indirect lane; only the fragment shading body changes. `RetroPass` gains histogram eye adaptation and an SPD-style bloom. No publication slots, leases, receiver windows, deadline prediction or gameplay-query receipt remain.

**Tech Stack:** C++23/C++20 modules, Slang shaders, engine RHI/Shader services, CMake presets, PPR unit-test framework.

**Spec:** `docs/superpowers/specs/2026-11-07-world-light-field-design.md`

**Backup checkpoint:** `6aa99aa lighting: backup pre-simplification working tree`. Do not rewrite history; commit each task below.

## Global Constraints

- Map sizes: 1024² baseline, 2048² maximum, 4096² aspirational benchmark only. 4096² is never a delivery gate.
- Refined/field spacing is 2 native cells at every shipping size. Do not silently coarsen it.
- Level 0: 4 directions. Level `l`: `2^(l+1)` spacing, `4^(l+1)` directions. Each level stores its own interval radiance; levels are not merged down.
- Temporal accumulation is per direction, in world space, with no camera reprojection.
- 60 Hz target; 6 ms field-update GPU budget at 1024²/2048². If over budget, lengthen the cycle; never change spacing or drop dirty work.
- No receiver windows, publication slots, leases, generation/coverage certificates, deadline scheduler or per-frame GPU readback.
- Terrain geometry is one 128² chunk quad (6 vertices), never per-cell geometry.
- Base normal +Z; material normal maps retained; macro open-edge normal deferred.
- Translucents keep refraction/absorption; refraction samples the background copy only. Particles/fluids are hard-clipped by opaque terrain.
- Palette/dither is off by default; one AgX tonemapper.
- Eye adaptation uses a GPU luminance histogram, ~0.7 s time constant, ±4 EV around manual exposure.
- Bloom uses an AMD-SPD-inspired single-pass mip downsampling and tent upsample.
- Only keep features the game uses or a required gate proves. Delete superseded code in the same series that makes its replacement default.
- Every task ends with a build, its focused tests, and a commit.

## Review Focus

- Camera pan/zoom/resize must never surface a lighting error, exit the application, or log per frame.
- A source edit must reset the affected probes and converge without resetting the whole field or leaving stale light indefinitely.
- Per-direction accumulation must stay finite and non-negative in dark, high-albedo and coloured-bounce scenes; history must be camera-independent.
- Translucent/particle pixels must never paint over opaque terrain, and normal-map shading must never emit an invalid normal.
- At 2048² the per-frame budget must never silently coarsen spacing, drop dirty work, or starve background refresh.

---

### Task 1: Stabilize Camera and Diagnostics

**Files:**
- Modify: `lib/engine/app/scene/App.Scene.Camera.Controller.cpp`
- Modify: `lib/engine/app/App.Application.Editor.cpp`
- Modify: `game/Game.Application.cpp`
- Test: `lib/engine/tests/app/App.Camera.Tests.cpp`
- Test: `lib/engine/tests/app/App.RunGate.Tests.cpp`

**Interfaces:**
- Consumes: existing `PanCameraController::setOrthoScale`, `setWorldBounds`, `setViewportSize`.
- Produces: `PanCameraController` clamps ortho scale so the visible rect cannot exceed the world bounds; `retainableTerrainViewError_` also accepts `function_not_supported`, `invalid_argument`, `device_or_resource_busy` and `timed_out`; per-frame world-radiance measurement logs are gated by an explicit measurement flag.

- [ ] **Step 1: Write the failing camera test**

Add to `App.Camera.Tests.cpp`:

```cpp
PPR_UNIT_TEST (ortho_scale_cannot_exceed_world_extent) {
    PanCameraController camera;
    PPR_TEST_ASSERT(not camera.setWorldBounds(float2{0, 0}, float2{128, 128}));
    camera.setViewportSize(float2{1920, 1080});
    PPR_TEST_ASSERT(not camera.setOrthoScale(1000.0f));
    const float max_scale = 128.0f / 1920.0f;
    PPR_TEST_ASSERT(camera.getOrthoScale() <= max_scale);
    PPR_TEST_ASSERT(camera.getOrthoScale() >= 0.001f);
}
```

`setViewportSize` returns `void`; `getOrthoScale()` is the existing accessor.

- [ ] **Step 2: Run the camera test to verify it fails**

Run: `out/build/clang-dev/lib/engine/tests/app/engine.tests.app --run-test app/camera/ortho_scale_cannot_exceed_world_extent --no-shuffle`
Expected: FAIL because `setOrthoScale(1000)` still succeeds.

- [ ] **Step 3: Implement the clamp and error/log policy**

In `App.Scene.Camera.Controller.cpp`, clamp in `setOrthoScale` and `applyAnchoredZoom_` to
`min(1000.0f, world_extent / max(viewport_extent, 1))` when both bounds and a valid viewport exist. Keep the existing invalid-argument behavior for non-finite/non-positive values.

In `App.Application.Editor.cpp`, extend `retainableTerrainViewError_` with the four codes above and route the two unretained `planWorldRadianceReceiverWindow` sites through the retained path (keep the old path alive until Task 13). Gate `WORLD_TEMPORAL_FRAME_ALLOWANCE` and admission logs behind an explicit measurement flag.

In `game/Game.Application.cpp`, stop calling `enableWorldRadianceMeasurements(true)` merely because `m_direct_window` is set; enable it only for the showcase/metrics/door/lamp flags.

- [ ] **Step 4: Run the affected tests**

Run: `out/build/clang-dev/lib/engine/tests/app/engine.tests.app --run-test app/camera --run-test app/run_gate --no-shuffle`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/engine/app/scene/App.Scene.Camera.Controller.cpp lib/engine/app/App.Application.Editor.cpp game/Game.Application.cpp lib/engine/tests/app/App.Camera.Tests.cpp
git commit -m "lighting: stabilize camera bounds and diagnostics"
```

---

### Task 2: Delete the Dormant Legacy Radiance Engine

**Files:**
- Delete: `lib/engine/app/renderer/App.Renderer.LightingPass.cppm`
- Delete: `lib/engine/app/renderer/App.Renderer.LightingPass.cpp`
- Delete: `lib/engine/app/renderer/App.Renderer.OpticalGeometry.cppm`
- Delete: `lib/engine/app/renderer/App.Renderer.OpticalGeometry.cpp`
- Delete: `assets/shaders/radiance_cascades.slang`, `radiance_cascades_refined.slang`, `radiance_terrain.slang`, `terrain_query_shared.slang`, `terrain_precision32.slang`, `terrain_precision64.slang`, `terrain_precision_refinement.slang`
- Delete: `lib/engine/tests/app/App.RadianceGpu.Tests.cpp`
- Modify: `lib/engine/app/CMakeLists.txt`, `lib/engine/app/App.cppm`, `lib/engine/tests/app/CMakeLists.txt`, `lib/engine/tests/app/App.Tests.cpp`
- Modify: `lib/engine/tests/app/App.WorldRadiance.Tests.cpp` (move the shared `backgroundReceivers_` helper here)
- Modify: `game/colony/LightingScene.Capture.cpp` (retire references)

**Interfaces:**
- Consumes: nothing.
- Produces: no production/test references to `RadianceCascadePass`, `OpticalGeometry`, or the legacy radiance shader family. `terrain_receiver_types.slang` stays until Task 13 because `world_radiance_types.slang` still includes it.

- [ ] **Step 1: Confirm no production references remain**

Run: `rg -n "RadianceCascadePass|OpticalGeometry|radiance_cascades|terrain_precision|terrain_query_shared" lib/engine/app game --glob '!out/**'`
Expected: only the files being deleted and the capture harness.

- [ ] **Step 2: Move the shared test helper and delete the files**

Move `backgroundReceivers_` from `App.RadianceGpu.Tests.cpp:1427` into `App.WorldRadiance.Tests.cpp` (or a small shared header) and keep `App.WorldRadiance.Tests.cpp` compiling until Task 13. Then delete the legacy pass/modules/shaders/test source and their CMake/exports/registrations. Retire the legacy capture command/harness; do not leave dead call sites.

- [ ] **Step 3: Build and run the remaining app tests**

Run: `cmake --build out/build/clang-dev --target engine.tests.app -j 8`
Run: `out/build/clang-dev/lib/engine/tests/app/engine.tests.app --run-test app/lighting --run-test app/grid_pass --no-shuffle`
Expected: build succeeds; tests PASS.

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "lighting: remove dormant radiance cascade engine"
```

---

### Task 3: World Light Plan Types

**Files:**
- Create: `lib/engine/app/renderer/App.Renderer.WorldLightTypes.cppm`
- Modify: `lib/engine/app/CMakeLists.txt`, `lib/engine/app/App.cppm`
- Create: `lib/engine/tests/app/App.WorldLightField.Tests.cpp`
- Modify: `lib/engine/tests/app/CMakeLists.txt`, `lib/engine/tests/app/App.Tests.cpp`, `lib/engine/tests/app/App.Tests.cppm` if needed

**Interfaces:**
- Produces:
  - `pP::lighting::WorldLightSettings { u32 m_width, m_height; float m_cell_metres; u32 m_level_count; u32 m_samples_per_frame; u32 m_accumulation_samples; float m_history_retention; float m_interval_scale; }`
  - `pP::lighting::WorldLightLevel { u32 m_index, m_spacing_cells, m_directions, m_probes_x, m_probes_y; u64 m_sample_offset, m_sample_count; float m_interval_near_m, m_interval_far_m; }`
  - `pP::lighting::WorldLightPlan { std::array<WorldLightLevel, 8> m_levels; u32 m_level_count; u32 m_width, m_height; float m_cell_metres; u64 m_radiance_samples, m_radiance_bytes, m_state_bytes; }`
  - `Expected<WorldLightPlan> planWorldLight(const WorldLightSettings&) noexcept;`

- [ ] **Step 1: Write the failing plan test**

In `App.WorldLightField.Tests.cpp`:

```cpp
PPR_UNIT_TEST (plan_1024_and_2048_use_spacing_two_and_equal_level_samples) {
    for (const u32 edge: {1024u, 2048u}) {
        const auto plan = lighting::planWorldLight({.m_width = edge, .m_height = edge, .m_cell_metres = 0.5f});
        PPR_TEST_ASSERT(plan.has_value());
        PPR_TEST_ASSERT(plan->m_levels[0].m_spacing_cells == 2u);
        PPR_TEST_ASSERT(plan->m_levels[0].m_directions == 4u);
        PPR_TEST_ASSERT(plan->m_levels[1].m_directions == 16u);
        PPR_TEST_ASSERT(plan->m_levels[2].m_directions == 64u);
        for (u32 index = 0; index < plan->m_level_count; ++index) {
            PPR_TEST_ASSERT(plan->m_levels[index].m_sample_count == plan->m_levels[0].m_sample_count);
        }
        PPR_TEST_ASSERT(plan->m_radiance_bytes == plan->m_radiance_samples * 8u);
    }
}
```

Add a rejection test for zero width, odd level counts exceeding 8, and non-finite cell size.

- [ ] **Step 2: Run the test to verify it fails**

Run: `out/build/clang-dev/lib/engine/tests/app/engine.tests.app --run-test app/world_light_field --no-shuffle`
Expected: FAIL because the group/module does not exist.

- [ ] **Step 3: Implement the plan and test registration**

Compute `spacing = 2 << level`, `directions = 4 << (2 * level)`, probes = `ceil(edge / spacing)`, sample count = probes² × directions. Intervals start at 0 and double with `m_interval_scale`; the coarsest far bound reaches at least `2 * (width + height) * cell_metres`. Reject overflow and invalid settings with `std::errc::invalid_argument`.

Register a `world_light_field` test group in `App.Tests.cpp` and a CMake shard:

```cmake
ppr_add_test_entry(TARGET engine.tests.app
    NAME world_light_field
    FILTER "app/world_light_field"
    LOOPS
    RESOURCE_LOCK gpu
    TIMEOUT 300
    LABELS unit engine app
)
```

- [ ] **Step 4: Run the test to verify it passes**

Run the Step 2 command. Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/engine/app/renderer/App.Renderer.WorldLightTypes.cppm lib/engine/app/CMakeLists.txt lib/engine/app/App.cppm lib/engine/tests/app/App.WorldLightField.Tests.cpp lib/engine/tests/app/CMakeLists.txt lib/engine/tests/app/App.Tests.cpp
git commit -m "lighting: add world light plan"
```

---

### Task 4: Dense World Source (CPU)

**Files:**
- Create: `lib/engine/app/renderer/App.Renderer.WorldLightSource.cppm`
- Create: `lib/engine/app/renderer/App.Renderer.WorldLightSource.cpp`
- Modify: `lib/engine/app/CMakeLists.txt`, `lib/engine/app/App.cppm`
- Test: `App.WorldLightField.Tests.cpp`

**Interfaces:**
- Produces:
  - `WorldLightMaterial { u32 m_kind; float m_source[3], m_sigma[3], m_emission[3]; }`
  - `WorldLightChunkUpdate { u32 m_chunk_id; std::span<const u16> m_cell_ids; }`
  - `WorldLightOverlayCell { u32 m_cell; float m_coverage; float m_sigma[3]; float m_emission[3]; }`
  - `WorldLightSourceUpdate { std::span<const WorldLightChunkUpdate> m_chunks; std::span<const WorldLightOverlayCell> m_overlay; u64 m_revision; i32 m_sky_hour; std::array<float,3> m_sky; }`
  - `WorldLightDirtyRect { i32 m_min_x, m_min_y, m_max_x, m_max_y; bool empty() const; void include(i32 x0, i32 y0, i32 x1, i32 y1); }`
  - `class WorldLightSource` with `create(WorldLightPlan, std::span<const WorldLightMaterial>)`, `applyUpdate(const WorldLightSourceUpdate&)`, `cellId(u32 x, u32 y) const`, `revision() const`, `dirtyRect() const`, `occupancyWord(u32 block, u32 word) const`.

- [ ] **Step 1: Write the failing source test**

```cpp
PPR_UNIT_TEST (source_applies_dirty_chunks_and_preserves_other_cells) {
    const auto plan = lighting::planWorldLight({.m_width = 256, .m_height = 256, .m_cell_metres = 0.5f});
    PPR_TEST_ASSERT(plan.has_value());
    const lighting::WorldLightMaterial materials[] = {{.m_kind = 1u}};
    auto source = lighting::WorldLightSource::create(*plan, materials);
    PPR_TEST_ASSERT(source.has_value());

    std::vector<u16> chunk(128u * 128u, 0u);
    chunk[0] = 1u;
    const lighting::WorldLightChunkUpdate update{.m_chunk_id = 0u, .m_cell_ids = chunk};
    PPR_TEST_ASSERT(not source->applyUpdate({.m_chunks = {&update, 1u}, .m_revision = 2u}));
    PPR_TEST_ASSERT(source->cellId(0u, 0u) == 1u);
    PPR_TEST_ASSERT(source->cellId(129u, 0u) == 0u);
    PPR_TEST_ASSERT(source->revision() == 2u);
    PPR_TEST_ASSERT(source->dirtyRect().m_min_x == 0 and source->dirtyRect().m_max_x == 128);
}
```

Also test an empty update keeps the revision and dirty rect, and an out-of-range chunk returns `result_out_of_range`.

- [ ] **Step 2: Run the test to verify it fails**

Run: `out/build/clang-dev/lib/engine/tests/app/engine.tests.app --run-test app/world_light_field/source_applies_dirty_chunks_and_preserves_other_cells --no-shuffle`
Expected: FAIL (module missing).

- [ ] **Step 3: Implement the CPU source**

Store a dense `std::vector<u16>` of `width*height`, the palette, a dense overlay `std::vector<WorldLightOverlayCell>` (or a sparse dirty map initially), the dirty rect, revision and sky. `applyUpdate` validates chunk ids/extents, copies ids at the chunk offset, includes the chunk in the dirty rect, applies overlay cells and bumps revision only when something changed. Keep it free of RHI.

- [ ] **Step 4: Run the tests to verify they pass**

Run the group. Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/engine/app/renderer/App.Renderer.WorldLightSource.* lib/engine/app/CMakeLists.txt lib/engine/app/App.cppm lib/engine/tests/app/App.WorldLightField.Tests.cpp
git commit -m "lighting: add dense world light source"
```

---

### Task 5: GPU Source, Matter Overlay and Occupancy

**Files:**
- Create: `lib/engine/app/renderer/App.Renderer.WorldLightSourceGpu.cppm`
- Create: `lib/engine/app/renderer/App.Renderer.WorldLightSourceGpu.cpp`
- Create: `assets/shaders/world_light_matter.slang`
- Create: `assets/shaders/world_light_source.slang`
- Modify: `lib/engine/app/CMakeLists.txt`, `lib/engine/app/App.cppm`
- Test: `App.WorldLightField.Tests.cpp`

**Interfaces:**
- Consumes: `WorldLightPlan`, `WorldLightSource`, `WorldLightSourceUpdate`.
- Produces:
  - `class WorldLightSourceGpu` with `initialize(IRhiService&, IShaderService&, const WorldLightPlan&, std::span<const WorldLightMaterial>, path content_dir)`, `applyUpdate(const WorldLightSourceUpdate&)`, `encode(rhi::ICommandEncoder&)`, `cellIds()`, `materials()`, `overlay()`, `occupancy()`, `revision()`, `dirtyRect()`, `shutdown()`.
  - Shader constants `WorldLightSourceConstants { uint width, height; uint chunkEdge; uint occupancyWords; uint revision; uint hasOverlay; uint pad[2]; }`.
  - Matter entry points `clearWorldLightMatter`, `scatterWorldLightMatter`, `finishWorldLightMatter`; occupancy entry `buildWorldLightOccupancy`.

- [ ] **Step 1: Write the failing GPU source test**

Using a new `WorldLightGpu::Fixture` modeled on `WorldRadianceGpu::Fixture` (`App.WorldRadiance.Tests.cpp:132`):
- initialize a 256² source;
- upload one chunk with a 1 at `(0,0)`;
- dispatch matter overlay for one opaque particle at cell `(4,4)` with coverage 1;
- read back the cell ids, overlay cell and occupancy bits;
- assert revision, dirty rect, and that a failed/empty update preserves the previous bytes.

- [ ] **Step 2: Run the test to verify it fails**

Run: `out/build/clang-dev/lib/engine/tests/app/engine.tests.app --run-test app/world_light_field/gpu_source_matter_overlay --no-shuffle`
Expected: FAIL (module/shader missing).

- [ ] **Step 3: Implement the GPU source and shaders**

`world_light_source.slang` builds/patch occupancy from dense ids + overlay for dirty blocks. `world_light_matter.slang` clears the dirty cell region, scatters particle fill/coverage-weighted optical terms with atomics, and finalizes rows (normalize opaque coverage, clamp, merge terrain optics). Host `encode` records clear/scatter/finalize/occupancy in that order with barriers. No radix sort, hash lookup or ancestor buffers.

- [ ] **Step 4: Run the test to verify it passes**

Run the Step 2 command. Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/engine/app/renderer/App.Renderer.WorldLightSourceGpu.* assets/shaders/world_light_source.slang assets/shaders/world_light_matter.slang lib/engine/app/CMakeLists.txt lib/engine/app/App.cppm lib/engine/tests/app/App.WorldLightField.Tests.cpp
git commit -m "lighting: add GPU world source and matter overlay"
```

---

### Task 6: Cascade Field Storage and Temporal Update

**Files:**
- Create: `lib/engine/app/renderer/App.Renderer.WorldLightField.cppm`
- Create: `lib/engine/app/renderer/App.Renderer.WorldLightField.cpp`
- Create: `assets/shaders/world_light_field.slang`
- Create: `assets/shaders/world_light_trace.slang`
- Modify: `lib/engine/app/CMakeLists.txt`, `lib/engine/app/App.cppm`
- Test: `App.WorldLightField.Tests.cpp`

**Interfaces:**
- Consumes: `WorldLightPlan`, `WorldLightSourceGpu`.
- Produces:
  - `WorldLightFieldConstants { float origin_x, origin_y; uint width, height; uint level_count; float cell_metres; uint source_revision; uint frame; uint budget; uint pad[3]; WorldLightLevelGpu levels[8]; }`
  - `WorldLightLevelGpu { uint spacing; uint directions; uint probe_offset; uint sample_offset; uint probes_x; uint probes_y; float near_m; float far_m; }`
  - `class WorldLightField` with `initialize(IRhiService&, IShaderService&, const WorldLightPlan&, path content_dir)`, `update(rhi::ICommandEncoder&, const WorldLightSourceGpu&, float dt, u32 sample_budget)`, `view() -> { radiance, previous_radiance, constants }`, `cycle()`, `cursor()`, `shutdown()`.
  - `updateWorldLightField` entry: one thread per `(probe, direction)`; reads previous radiance, traces the level interval, composites previous-field feedback at hits, updates the per-direction mean/count, writes current radiance.

- [ ] **Step 1: Write the failing field test**

GPU fixture, 256² map, a single opaque emissive cell:
- run 16 updates with budget ≥ one full level;
- read a probe direction near the emitter and assert finite, positive radiance and a monotonic sample count;
- assert `cycle()` advanced and `cursor()` wrapped within the budget;
- run the same 16 updates with a different camera snapshot and assert the radiance bytes are identical;
- turn the emitter off via a dirty update and assert only intersecting probes reset (`count == 0`) while a distant probe keeps its count.

- [ ] **Step 2: Run the test to verify it fails**

Run: `out/build/clang-dev/lib/engine/tests/app/engine.tests.app --run-test app/world_light_field/field_single_emitter_converges --no-shuffle`
Expected: FAIL (module/shader missing).

- [ ] **Step 3: Implement field storage and update**

Allocate one radiance buffer per ping-pong (8 B/sample), one state buffer (per-direction count), and the constants. Directions are `(index + 0.5) * tau / directions`; sample jitter comes from a per-probe/per-frame hash. Intervals are level-local. The trace reads the dense source + occupancy + overlay, and hit shading reads the previous hierarchy composite. Dispatch one range per level, dirty ranges first, then the round-robin cursor. Never read back; no publication.

- [ ] **Step 4: Run the test to verify it passes**

Run the Step 2 command. Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/engine/app/renderer/App.Renderer.WorldLightField.* assets/shaders/world_light_field.slang assets/shaders/world_light_trace.slang lib/engine/app/CMakeLists.txt lib/engine/app/App.cppm lib/engine/tests/app/App.WorldLightField.Tests.cpp
git commit -m "lighting: add temporal world light field"
```

---

### Task 7: Composite Sampling and Material Response

**Files:**
- Create: `assets/shaders/world_light_sample.slang`
- Modify: `lib/engine/app/renderer/App.Renderer.WorldLightField.cppm/.cpp` (expose composite test entry)
- Test: `App.WorldLightField.Tests.cpp` (CPU/GPU reference)

**Interfaces:**
- Consumes: `WorldLightFieldConstants`, radiance/current + constants.
- Produces:
  - `float3 worldLightComposite(float2 position, float2 direction)` — bilinear per level, near-to-far transmittance composite.
  - `float3 worldLightDiffuse(float2 position, float3 normal)` — cosine-weighted small direction set.
  - `float3 worldLightSpecular(float2 position, float3 reflection, float roughness)` — reflection direction (plus a small lobe).
  - `bool worldLightProbe(const WorldLightLevelGpu&, int2 probe, uint direction, out float3 radiance, out float transmittance)`.

- [ ] **Step 1: Write the failing composite test**

Add a GPU entry that writes `worldLightComposite` for a known synthetic hierarchy into a buffer. Build the same hierarchy on the CPU in the test (a few direction samples with known radiance/transmittance) and assert the GPU result matches within `1e-4`. Add a normals case: a +Z normal returns the diffuse near-isotropic term; a normal-map-perturbed normal changes the result and never produces NaN.

- [ ] **Step 2: Run the test to verify it fails**

Run: `out/build/clang-dev/lib/engine/tests/app/engine.tests.app --run-test app/world_light_field/composite_matches_cpu_reference --no-shuffle`
Expected: FAIL (shader/include missing).

- [ ] **Step 3: Implement the sampling include**

Implement the composite as `L = L0 + T0*(L1 + T1*(...))`, bilinear across probes per level. Diffuse uses a deterministic 8-direction set with cosine weights; specular uses the reflection direction and a roughness-scaled lobe. Clamp only the final physical output. Expose a test entry that writes the result for one position/direction.

- [ ] **Step 4: Run the test to verify it passes**

Run the Step 2 command. Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add assets/shaders/world_light_sample.slang lib/engine/app/renderer/App.Renderer.WorldLightField.cppm lib/engine/app/renderer/App.Renderer.WorldLightField.cpp lib/engine/tests/app/App.WorldLightField.Tests.cpp
git commit -m "lighting: add world light composite sampling"
```

---

### Task 8: Opaque Terrain Shading Through the Chunk Lane

**Files:**
- Modify: `assets/shaders/grid_tiles.slang`
- Modify: `lib/engine/app/renderer/App.Renderer.GridPass.cppm/.cpp`
- Modify: `lib/engine/app/renderer/App.Renderer.GridPass.Particles.cpp` (binding only)
- Test: `lib/engine/tests/app/App.GridPass.Tests.cpp`, `App.WorldLightField.Tests.cpp`

**Interfaces:**
- Consumes: `WorldLightField::view()`.
- Produces: `GridPass::setWorldLightField(const WorldLightFieldView&)`; fragment body `shadeWorldLightSurface` using the cell atlas, material row, albedo/normal maps, `worldLightDiffuse`/`worldLightSpecular`, and self-emission.

- [ ] **Step 1: Write the failing shading test**

Add a GPU test with a uniform emissive field and a simple two-cell chunk:
- render one frame through `GridPass` with the field bound;
- read back the HDR color and assert it is finite, non-zero and differs between a lit and unlit field;
- assert the material normal map changes the shaded result;
- assert the vertex path still issues one quad per 128² chunk (indirect geometry count unchanged).

- [ ] **Step 2: Run the test to verify it fails**

Run: `out/build/clang-dev/lib/engine/tests/app/engine.tests.app --run-test app/world_light_field/grid_shading_uses_field --no-shuffle`
Expected: FAIL (no field binding/shading body).

- [ ] **Step 3: Implement the shading**

Replace the `Main/Spatial/Reactive` duplicated bodies with one shared function and thin wrappers. Keep the plain, `SV_SampleIndex` spatial and R8 reactive entries. Sample cells from the existing atlas, decode albedo + normal maps with `SampleGrad`, evaluate the field composite, add linear self-emission once, and write HDR. Do not touch the geometry/upload/cull lane.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `out/build/clang-dev/lib/engine/tests/app/engine.tests.app --run-test app/grid_pass --run-test app/world_light_field/grid_shading_uses_field --no-shuffle`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add assets/shaders/grid_tiles.slang lib/engine/app/renderer/App.Renderer.GridPass.* lib/engine/tests/app/App.GridPass.Tests.cpp lib/engine/tests/app/App.WorldLightField.Tests.cpp
git commit -m "lighting: shade terrain from the world field"
```

---

### Task 9: Translucents, Fluids and Terrain Clipping

**Files:**
- Modify: `assets/shaders/grid_tiles.slang` (translucent/particle/fluid entries)
- Modify: `lib/engine/app/renderer/App.Renderer.GridPass.cppm/.cpp` (pass order and state)
- Test: `App.WorldLightField.Tests.cpp`

**Interfaces:**
- Consumes: same field/material sampling.
- Produces: opaque → particles/fluids (clipped) → translucent terrain pass order; background-only refraction; Beer–Lambert tint.

- [ ] **Step 1: Write the failing clipping/blending test**

GPU test:
- place a particle quad over an opaque terrain cell; assert its pixels are discarded (terrain color unchanged);
- place it over a vacuum cell; assert it blends;
- place a translucent cell with IOR/absorption; assert the background offset/tint changes the readback versus a flat alpha blend;
- assert no particle fragment survives where the underlying native cell is opaque.

- [ ] **Step 2: Run the test to verify it fails**

Run: `out/build/clang-dev/lib/engine/tests/app/engine.tests.app --run-test app/world_light_field/translucent_clipped_by_terrain --no-shuffle`
Expected: FAIL (current pass order/clipping).

- [ ] **Step 3: Implement the passes**

Keep the chunk geometry; split opaque/particles/translucent into the specified order with depth states. Particle/fluid fragments resolve the native cell and discard on opaque terrain. Translucents sample the background copy with the IOR/normal offset and apply the absorption tint; do not sample the scene color or add a second light source. Keep the existing liquid surface reconstruction.

- [ ] **Step 4: Run the test to verify it passes**

Run the Step 2 command. Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add assets/shaders/grid_tiles.slang lib/engine/app/renderer/App.Renderer.GridPass.* lib/engine/tests/app/App.WorldLightField.Tests.cpp
git commit -m "lighting: shade translucents and clip particles"
```

---

### Task 10: Route the Field in the Game and Producer

**Files:**
- Modify: `lib/engine/app/App.Application.Editor.cppm/.cpp`
- Modify: `game/Game.Application.cpp`
- Modify: `game/Game.Config.cppm/.cpp`
- Modify: `game/colony/SimFramePacket.cpp`
- Modify: `lib/engine/app/renderer/App.Renderer.WorldLightField.*` (view/identity)
- Test: `App.WorldLightField.Tests.cpp`, `game/Game.Config.Tests.cpp`, `App.RunGate.Tests.cpp`

**Interfaces:**
- Consumes: `WorldLightSourceUpdate`, `WorldLightField`.
- Produces: a `field` lighting mode; `RenderFrameResult::m_lighting_source_revision` and optional `m_lighting_field_cycle`; `GridPass::setWorldLightField`; producer dirty-chunk updates only.

- [ ] **Step 1: Write the failing routing tests**

- Config test: `--world-lighting=field` parses and is rejected only with an invalid map size.
- GPU/integration test: commit a dirty chunk, run one editor frame, assert `sourceRevision` advances and the field view is bound; assert the stage includes only dirty chunks (not all chunks) on an ordinary edit.
- Camera test: pan/zoom across the world with the field mode and assert no lighting error/log registration and no frame error.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `out/build/clang-dev/lib/engine/tests/app/engine.tests.app --run-test app/world_light_field --no-shuffle`
Run: `out/build/clang-dev/game/game.tests --run-test game/config --no-shuffle`
Expected: FAIL (mode/routing missing).

- [ ] **Step 3: Implement the routing**

Add the mode/settings. On each committed stage, build a `WorldLightSourceUpdate` from the dirty chunk list and committed Matter particles; no full `WorldRadianceSource::capture`. Update the field before the scene pass. Keep the old mode selectable until Task 13. Remove receiver-window demand planning from the field mode and gate all remaining diagnostics.

- [ ] **Step 4: Run the tests to verify they pass**

Run the Step 2 command. Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/engine/app/App.Application.Editor.* game/Game.Application.cpp game/Game.Config.* game/colony/SimFramePacket.cpp lib/engine/app/renderer/App.Renderer.WorldLightField.* lib/engine/tests/app/App.WorldLightField.Tests.cpp game/Game.Config.Tests.cpp lib/engine/tests/app/App.RunGate.Tests.cpp
git commit -m "lighting: route world light field in the game"
```

---

### Task 11: World Light and Post Display Settings

**Files:**
- Modify: `game/Game.Config.cppm/.cpp`
- Modify: `game/Game.Display.cppm/.cpp` (overlay persistence)
- Modify: `game/ui/ColonyPanel.cpp` (Display window section)
- Modify: `lib/engine/app/ui/widgets/App.UI.Widgets.Display.cppm/.cpp` if the shared widget owns the section
- Modify: `game/Game.Application.cpp` (apply settings)
- Test: `game/Game.Config.Tests.cpp`, `game/Game.Display.Tests.cpp`, `game/ui/Game.UI.Tests.cpp`

**Interfaces:**
- Produces:
  - `WorldLightDisplaySettings { bool m_enabled; u32 m_samples_per_frame; u32 m_accumulation_samples; float m_history_retention; u32 m_level_count; i32 m_sky_hour; }`
  - `PostDisplaySettings { bool m_auto_exposure; float m_exposure_ev; float m_adaptation_tau; float m_ev_min; float m_ev_max; bool m_bloom; float m_bloom_intensity; float m_bloom_threshold; bool m_palette; }`
  - CLI/config keys, overlay persistence, and a Display-window section exposing them.
  - Runtime application to `WorldLightField` (budget, accumulation, retention), `RetroPass` (auto exposure, EV range/tau, bloom, palette).

- [ ] **Step 1: Write the failing settings tests**

- Config parse test: valid `--world-light-samples-per-frame`, `--world-light-accumulation`, `--post-auto-exposure`, `--post-exposure-ev`, `--post-bloom-intensity`, `--post-palette`; invalid ranges rejected.
- Overlay round-trip test: write/read `user-settings.toml`, preserve unrelated entries, defaults for absent keys, palette false by default.
- UI draft test: toggles/sliders produce valid settings; reset restores defaults; an invalid draft is rejected with diagnostics.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `out/build/clang-dev/game/game.tests --run-test game/config --run-test game/display --run-test game/ui --no-shuffle`
Expected: FAIL (settings missing).

- [ ] **Step 3: Implement the settings and panel**

Add the two structs to the game settings schema with ranges/defaults: samples per frame 65,536–4,194,304; accumulation 1–64; retention 0.5–1.0; level count 4–8; exposure EV −16..16; tau 0.1–5 s; EV clamp within −8..8; bloom intensity 0–2; threshold 0–16; palette default false. Add CLI options and overlay persistence under `game.world_light.*` and `game.post.*`. Add a "Lighting & Post" section to the Display window with runtime-apply controls and Reset. Apply the values to the field and RetroPass; the fixed-exposure override disables adaptation for captures.

- [ ] **Step 4: Run the tests to verify they pass**

Run the Step 2 command. Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add game/Game.Config.* game/Game.Display.* game/ui/ColonyPanel.cpp lib/engine/app/ui/widgets/App.UI.Widgets.Display.* game/Game.Application.cpp game/Game.Config.Tests.cpp game/Game.Display.Tests.cpp game/ui/Game.UI.Tests.cpp
git commit -m "lighting: add world light and post display settings"
```

---

### Task 12: Histogram Eye Adaptation

**Files:**
- Create: `assets/shaders/luminance_histogram.slang`
- Create: `assets/shaders/auto_exposure.slang`
- Modify: `lib/engine/app/renderer/App.Renderer.RetroPass.cppm/.cpp`
- Test: `App.WorldLightField.Tests.cpp`

**Interfaces:**
- Consumes: resolved HDR scene after temporal2x.
- Produces: a small luminance texture, a 64–128-bin histogram buffer, a 1×1 adapted-exposure texture; `RetroPass` samples the exposure in bloom/present; manual EV offset; fixed-exposure override.

- [ ] **Step 1: Write the failing histogram test**

- CPU test for log-bin mapping and percentile reduction with synthetic luminances (one black frame, one uniform 0.18, one dominated by a bright emitter) and assert the target EV ordering and clamp.
- GPU smoke: run the histogram/adaptation for two frames and assert finite exposure, bounded change, and no readback requirement.

- [ ] **Step 2: Run the test to verify it fails**

Run: `out/build/clang-dev/lib/engine/tests/app/engine.tests.app --run-test app/world_light_field/histogram_adaptation --no-shuffle`
Expected: FAIL (shaders/pass missing).

- [ ] **Step 3: Implement adaptation**

Downsample the resolved HDR scene to 64×64; build the log-luminance histogram with workgroup-local bins merged globally; reduce a percentile-clipped average to the target EV; temporally adapt with `1 - exp(-dt / 0.7)`, clamp ±4 EV around manual EV. Keep the state on the GPU; present samples it.

- [ ] **Step 4: Run the test to verify it passes**

Run the Step 2 command. Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add assets/shaders/luminance_histogram.slang assets/shaders/auto_exposure.slang lib/engine/app/renderer/App.Renderer.RetroPass.* lib/engine/tests/app/App.WorldLightField.Tests.cpp
git commit -m "lighting: add histogram eye adaptation"
```

---

### Task 13: SPD-Style Bloom

**Files:**
- Create: `assets/shaders/spd_bloom.slang`
- Delete: `assets/shaders/retro_bloom.slang`
- Modify: `lib/engine/app/renderer/App.Renderer.RetroPass.cppm/.cpp`
- Test: `App.WorldLightField.Tests.cpp`

**Interfaces:**
- Consumes: adapted exposure, resolved HDR scene.
- Produces: `prefilterBloom`, `downsampleBloom` (single-pass multi-mip), `upsampleBloom`, `compositeBloom`; FP16 mip chain; present adds bloom * intensity.

- [ ] **Step 1: Write the failing bloom test**

GPU test with a uniform HDR input and one bright block:
- assert finite output, non-zero bloom around the bright block, and a wider falloff than a single 5×5 blur;
- assert a black input stays black;
- assert the mip chain extents and cumulative bytes match the plan.

- [ ] **Step 2: Run the test to verify it fails**

Run: `out/build/clang-dev/lib/engine/tests/app/engine.tests.app --run-test app/world_light_field/spd_bloom --no-shuffle`
Expected: FAIL (shader/pass missing).

- [ ] **Step 3: Implement the bloom**

Bright-pass with soft knee/Karis weighting at half or quarter res; a single dispatch generates several mips with group-shared 2×2 reduction and odd-extent handling; a tent upsample accumulates each level back; present adds the final bloom once. Remove the old extract/blur path.

- [ ] **Step 4: Run the test to verify it passes**

Run the Step 2 command. Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add assets/shaders/spd_bloom.slang lib/engine/app/renderer/App.Renderer.RetroPass.* lib/engine/tests/app/App.WorldLightField.Tests.cpp
git commit -m "lighting: add SPD bloom"
```

---

### Task 14: Flip the Default and Retire the Old World Radiance Path

**Files:**
- Delete: `lib/engine/app/renderer/App.Renderer.WorldRadiance.cppm/.cpp`
- Delete: `assets/shaders/world_radiance.slang`, `world_radiance_types.slang`, `terrain_receiver_types.slang`, `grid_world_radiance.slang`, `world_matter_project.slang`, `world_matter_types.slang`, `grid_screen_contact_types.slang`
- Delete: `lib/engine/tests/app/App.WorldRadiance.Tests.cpp`
- Modify: `lib/engine/app/CMakeLists.txt`, `lib/engine/app/App.cppm`, `lib/engine/tests/app/CMakeLists.txt`, `App.Tests.cpp`
- Modify: `game/Game.Config.cppm/.cpp`, `game/Game.Application.cpp`, `game/colony/SimFramePacket.cpp`
- Modify: codemaps and `--world-lighting` docs

**Interfaces:**
- Consumes: validated field path.
- Produces: one production lighting mode; no `WorldRadiancePass`, `WorldRadianceSource`, receiver window, publication, lease, skip table, face/bounce or gameplay-query receipt.

- [ ] **Step 1: Confirm the field path passes the focused suite and captures**

Run the `world_light_field` suite, the 1024²/2048² captures, and the pan/zoom/edit runtime check from Task 10. Expected: PASS before deletion.

- [ ] **Step 2: Flip the default and delete**

Set `temporal_cascades`/`direct_window` routing to the field path; remove the old enum entries only if no external preset/tests depend on them. Delete the old pass, source, shaders, tests and registrations. Keep the CPU oracle types only if a retained test uses them; otherwise delete.

- [ ] **Step 3: Build and run the app test set**

Run: `cmake --build out/build/clang-dev --target engine.tests.app app.game -j 8`
Run: `ctest --test-dir out/build/clang-dev -R "world_light_field|app/grid_pass|app/camera|app/frame_fence" --output-on-failure`
Expected: PASS; no references to deleted symbols.

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "lighting: retire legacy world radiance"
```

---

### Task 15: Validation, Performance and Captures

**Files:**
- Modify: tests if a discovered contract needs a focused assertion.
- Modify: `docs/superpowers/specs/2026-11-07-world-light-field-design.md` only if an accepted decision changes.
- No production changes unless a measured failure requires one.

- [ ] **Step 1: Format and style gates**

Run: `python scripts/format_cpp.py`
Run: `python scripts/check_semantic_style.py --files <touched C++ files>`
Expected: exit 0; fix and re-run if not.

- [ ] **Step 2: Ordinary and GPU-assisted validation**

Run: `ctest --test-dir out/build/clang-dev -R "world_light_field|app/grid_pass|app/camera|app/frame_fence|app/render_lane" --output-on-failure`
Run the ordinary and GPU-assisted field scene once on the final binary/shader/settings identity. Do not add or relax timeouts to make a failure disappear.

- [ ] **Step 3: Release performance matrix**

Build `app.game` in `out/build/clang-rel`; run 1024², 2048² and 4096² with the final settings. Record per-frame field GPU time, cycle length, source upload bytes, scene time, total frame time and field/scene/whole-process memory. Targets: 6 ms field budget at 1024/2048, 8/16-frame cycles, camera pan/zoom no exit, edits visible within one affected-probe cycle.

- [ ] **Step 4: Captures and visual review**

Capture at 1024² and 2048²: dark single-emitter scene, daylight scene, door/lamp showcase, and the translucent/refraction case. View the images; check finite output, no magenta invalid rows, particle clipping and a readable light falloff.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "lighting: validate world light field"
```

---

## Self-Review

- **Spec coverage:** Each spec section maps to Task 1 (stability), 3 (plan), 4–5 (source), 6 (field), 7 (sampling), 8–9 (shading/translucency), 10 (routing), 11 (display settings), 12–13 (post), 14 (retirement), 15 (validation).
- **Type consistency:** `WorldLightPlan` is defined in Task 3 and consumed unchanged in Tasks 4–6; `WorldLightSourceUpdate` in Task 4 is consumed by Task 5 and Task 10; `WorldLightFieldView` is consumed by Task 8 and Task 10; `WorldLightFieldConstants`/`WorldLightLevelGpu` are shared by Tasks 6–7; `WorldLightDisplaySettings`/`PostDisplaySettings` in Task 11 feed Tasks 6, 10, 12 and 13.
- **Review focus:** pan/zoom is covered by Task 1 and Task 10; edit reset by Task 6; accumulation stability/camera independence by Task 6; clipping and normal validity by Task 8–9; budget/starvation by Task 6 and Task 15; invalid display settings by Task 11.
- **Proportion:** the plan specifies interfaces and test outcomes, not shader bodies or full implementations; the implementer writes the bodies.
