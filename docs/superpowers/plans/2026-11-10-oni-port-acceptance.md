# ONI port — final integrated verification and owner acceptance evidence

Date: 2026-11-12
Branch: `particles`, base revision `dee0168221af7db162f39c7be582b68f32bda4b2`
(`docs: record profiling completion and begin final verification`)
Host: Linux `pPd` (CachyOS), 32 logical CPUs, 62 GiB RAM, NVIDIA GeForce RTX 2080
(TU104), Vulkan, driver `620.70.06` (Vulkan API 1.4.357).
Status: **owner visual acceptance pending.** This document records verification
evidence; it does not claim final delivery and Task 19 is not marked complete.

Working tree: the user's unstaged `assets/worldgen/stylized/` work is preserved
and untouched. One Task 19 diagnostic fix is present uncommitted:
`assets/shaders/world_lighting_ch.slang` (initialize the `out CH` result on the
numeric-failure path; details in "Execution-log audit"). No other tracked change
was made.

## Spec coverage

| Spec responsibility | Owning task | Final evidence |
| --- | --- | --- |
| §1–3 scope, preservation, recovery, cleanup and acceptance | 0–3,19 | Recovery archive outside Git and stash object unchanged (§ YAGNI/naming); cleanup inventory in the execution ledger |
| §4 codecs/numeric status/backend support | 4,5,7,13,14 | CTest matrix; backend GPU-validation entries |
| §4 angular schedule/coarse/terminal sky/acceleration | 8,9 | CTest + Lab captures sky −1/12 |
| §4 fine fitting/cold start/EMA/in-place dependencies | 9,10,14 | CTest + log audit; CH shader fix |
| §4 queue/publication/stage settlement and teardown | 6,7,13,14,19 | Ordinary/Lab/display/profiler shutdown logs |
| §4–5 visible CH filters, BRDF and material semantics | 5,9,11 | CTest; Lab overview/inspection captures |
| §5 packed temperature/source palette/winner reduction/gas | 5–7,9,11 | CTest; gas/actor present in Lab world |
| §6 exact lab, ordinary loading/tools/save/load,1024 admission | 15,19 | Lab runs at 1024 seed 42; `--worldgen-acceptance` PASS |
| §7 independent post, arbitrary extents, temporal Hable | 2,4,11–14 | Display exercise 91 passed (P1/2/3/8 and odd extents) |
| §7 retained settings/inspection/display exercise | 1,2,16,17 | Display exercise JSON + settings file; inspection captures |
| §8 maintained Nsight interfaces/artifacts/ranges/analysis | 7–12,14,18,19 | Fresh profiling captures/analysis (§ Profiling) |
| §9 verification, blockers, host limits and owner acceptance | 3,4,19 | This document; owner acceptance pending |

## YAGNI and naming audit

Audit scope: all tracked active files (3,477 paths) excluding `docs/`,
`third_party/` and the content provenance note; plus build/tooling/fixture text.

- **Reference-project naming: zero.** Token-boundary search for `oni`/`Oni`/`ONI`
  in active filenames and contents returns no hits. Unrelated substrings
  (`monitor`, `poisoning`) are not matches. Remaining mentions are documentary
  only: `docs/oni_v22.slang` and the plans/specs, the content
  `assets/worldgen/default/PROVENANCE.md`, and the captured vendor export
  `scripts/tests/fixtures/lighting-profile/graphics*/BASE_UNLOCKED/REPRO_INFO.xls`,
  whose recorded command line embeds the artifact directory name
  `2026-11-10-oni-port`. The fixture is untouched real vendor output; editing it
  would falsify the export.
- **Retired paths: none active.** No radiance-atlas, atlas dilation/repair,
  `RetroPass`, AA selector/MSAA-mode setting, post palette, lighting
  ray-budget/cascade-count, or interval-field compatibility alias survives.
  `spatial_msaa4.slang` remains only as the approved sample-pattern
  qualification asset (asset gate). `game/Game.Config.Tests.cpp` asserts that
  `post-palette`, `world-light-samples-per-frame`, `world-light-debug`,
  `world-light-atlas-ambient` and `world-lighting` are absent from help/rejection.
  The `[palette]` tables in worldgen assets are content material palettes, not
  the retired renderer palette. "dirty"/"atlas" hits under `renderer/` are the
  pre-existing chunk/tile-upload mechanism, not lighting dirty/repair paths.
- **No fixed key or ambient floor.** `world_lighting_source.slang` computes
  `emission + thermal + diffuse * priorAngularMean` with no additive ambient;
  the only "ambient floor" mention is `grid_tiles.slang` documenting that the
  path adds none. Sky −1 is the black boundary sky.
- **No raw-L0 or per-hit BRDF feedback.** Tracing returns the cached RGB9E5
  source only; the cached source samples the prior fine CH DC field
  (diffuse-only). `world_lighting_source.slang` states coarse and fine tracing
  never evaluate a BRDF, normal, roughness or Fresnel factor.
- **No count-scaled particle emission.** Gas/particles are alpha-blended and
  lit by the CH mean; `grid_particle_prepare.slang`/`grid_particle_native.slang`
  contain no emission or particle-count radiance scaling.
- **No compatibility aliases** beyond the established canonical/leaf CLI schema
  (`game.<group>.<name>` with the kebab-case alias). New options are
  `world-light-sky-hour`, `world-light-diagnostic`, `world-light-cascade`,
  `pixel-scale`, `post-auto-exposure`, `post-exposure-ev`,
  `post-adaptation-tau`, `post-ev-min`, `post-ev-max`, `post-bloom`,
  `post-bloom-intensity`, `post-bloom-threshold`, `exercise-display`,
  `exercise-output`, `profile-capture`, `profile-output`, `profile-workload`.
- **New owners/APIs/settings/resources have current consumers.** `WorldLightingPass`
  (editor lifecycle, reconstruction/settlement, inspection overlay),
  `PresentationGrid`/`makePresentationGrid` (reconstruction, display exercise,
  post tests), `PostProcessPass` (editor lifecycle, reconstruction A/B/C),
  `ReconstructionPass` (editor lifecycle, post, display exercise),
  `DisplayExercise` (`--exercise-display`), `ProfileCapture`
  (`--profile-capture`), `rhi::getNativeQueueObject` (Graphics SDK capture only),
  the three `game.world_light.*`, `game.pixel.*` and `game.post.*` settings
  (CLI, display panel, persistence, exercise, renderer), and every Lab material
  (world record + content tests). Per-task reviews recorded the YAGNI verdicts;
  no speculative mode, generic pass graph, pass registry or alternate backend
  exists.
- **Dead/temporary code:** none found in the port diff
  (`git diff 49a3a7d..HEAD`): no `TODO`/`FIXME`/`HACK` markers in production
  files. Pre-existing `prepareLightingShowcase`/`lighting_showcase` predates the
  checkpoint (`6aa99aac`, 2026-11-08) and has a current GPU-test consumer; it is
  outside this port's YAGNI boundary.
- **Recovery and stash.** `/home/bek/ppr-recovery/lighting-precleanup/` remains
  outside Git and is not referenced by build, tooling or source. The paused stash
  object `8271a171148a99fca684a168a8c9acea48d2954c`
  (`task-6b paused: atlas dilation (pending cascade fix)`) is unchanged; it was
  not popped, dropped or rewritten.
- **Deferred minors from Task 17/18** (`ReconstructionHistoryStamp` padding,
  telemetry canary wording, fixture docs wording, `.pct` unit label, test-local
  `post.present` label) remain non-functional documentation/test-label items.
  They do not affect the gates below and are ruled deferred to a documentation
  cleanup, not silently fixed here.

## Clean builds

Both presets were configured and built from scratch (`--clean-first`) at
`dee01682` before the shader fix, with complete configure/build stdout+stderr
retained. Toolchain: `clang version 23.1.4`, CMake 4.4.5, Ninja 1.13.2,
vcpkg triplet `x64-linux-libcxx`. Both compile with `-Werror` (developer mode
for `clang-dev`, `PPR_WARNINGS_AS_ERRORS=ON` for `clang-rel`).

| Preset | Command | Exit | Edges | Log |
| --- | --- | --- | --- | --- |
| `clang-dev` | `cmake --preset clang-dev` | 0 | configure | `/tmp/opencode/task19/build-clang-dev/configure.log` |
| `clang-dev` | `cmake --build --preset clang-dev --clean-first -j 16` | 0 | 1738 | `/tmp/opencode/task19/build-clang-dev/build.log` |
| `clang-rel` | `cmake --preset clang-rel` | 0 | configure | `/tmp/opencode/task19/build-clang-rel/configure.log` |
| `clang-rel` | `cmake --build --preset clang-rel --clean-first -j 16` | 0 | 1738 | `/tmp/opencode/task19/build-clang-rel/build.log` |

Full-log inspection result:

- **Zero compilation/link errors** and **zero warnings from project code** in
  both presets (all project translation units use `-Werror`).
- **Two warnings in each preset from the system libc++ module object edges**:
  `/usr/share/libc++/v1/std.cppm:163:15: warning: 'std' is a reserved name for a
  module [-Wreserved-module-identifier]` and the identical
  `std.compat.cppm:80:15` warning. They are emitted by the CMake-synthesized
  `@cmake_cxx_std` object compile (`/usr/bin/clang++ … -c /usr/share/libc++/v1/std.cppm`),
  which does not inherit the target warning flags. The repository already
  applies `-Wno-reserved-module-identifier` to the BMI-precompile edge
  (`cmake/compiler/Clang.cmake:25`, documented in `cmake/compiler/codemap.md`)
  for exactly this vendor source; the object edge remains unsuppressed.
  This is pre-existing toolchain behavior exposed by a full clean build, not
  port code.
- **Zero-warning gate: UNMET as literally specified.** Hiding it by adding a
  suppression to the dependency would violate the owner's "do not exempt a
  dependency" rule; the exact bounded remedy (route the existing approved
  suppression to the std-module object edge, or accept the vendor diagnostic)
  needs an owner/coordinator ruling. `clang-rel` additionally reported no other
  warnings; configure reported only informational status (including
  `cppcheck requested but executable not found`, which is not a compiler
  warning).
- Incremental rebuilds after the shader fix only re-staged assets
  (`rebuild-after-shader-fix.log`, exit 0, no compile warnings).

Shader-cache state: runtime persistent shader cache `.ppr-shader-cache/v1`
(52 entries, 1.2 MiB, gitignored) was warm for the captured runs; it is a
backend-codegen/startup aid and is not part of build success.

## Routine and GPU-validation matrix

Inventory (`ctest --test-dir out/build/clang-dev -N`): 68 registered entries,
32 labeled `gpu_validation`; routine invocation selected 36 entries (groups and
leaves) with `-LE gpu_validation`.

| Run | Command | Exit | Result |
| --- | --- | --- | --- |
| Inventory | `ctest --test-dir out/build/clang-dev -N` | 0 | 68 entries; log `/tmp/opencode/task19/logs/ctest-inventory.txt` |
| Routine | `ctest --test-dir out/build/clang-dev -j4 --output-on-failure -LE gpu_validation` | 0 | **36/36 passed**, 1036.35 s wall; log `…/ctest-routine.log` |
| GPU validation | `ctest --test-dir out/build/clang-dev -L gpu_validation --output-on-failure` (serialized) | 8 | **28/32 passed**, 1510.93 s wall; log `…/ctest-gpu-validation.log` |

GPU-validation failures and disposition (full detail in the log):

| Entry | Failure | Disposition |
| --- | --- | --- |
| `engine.tests.app.world_lighting_settlement_gpu_validation` | `editor_mid_history_edits_do_not_reset_or_contaminate` read `reconstruction.sceneTextureForTest()`, which is a 4x MSAA target; Slang-RHI's debug layer rejects `readTexture` on MSAA (`RHI.cpp(116): Multisample textures cannot be read`). The invalid readback only surfaced under validation. | **Fixed in the test**: the MSAA scene pointer remains an existence check; the accepted single-sample Hable history is the committed screen-content byte evidence (it was already compared). Reruns: GPU twin 1/1 passed, 322 s, exit 0 (`…/ctest-settlement-gpu-rerun.log`); routine twin 1/1 passed, 314 s, exit 0 (`…/ctest-settlement-routine-rerun.log`). |
| `engine.tests.app.matter_material_gpu` | `app/matter_gpu/evolved_liquid_pool_has_no_interior_render_holes` deterministically fails with `holes=11144/11144`. The QA capture is uniform clear colour; `GridPass::renderGpuParticles_` returns early because the fixture's materials are liquid (`envelopes_only`), and Task 11 deliberately retired the solid/liquid envelope draw. | **Explained: stale test coverage of a retired rendering path**, not a production lighting defect. The leaf needs retirement/re-scope by its owning task; not modified here. Evidence: `…/logs/matter-pool-individual.log`, `…/logs/matter-pool-qa2.log`, PPMs under `/tmp/opencode/task19/matter-pool-qa/`. |
| `engine.tests.app.matter_runtime_scene_gpu` | After three map-edge runs, `SLANG_RHI_ASSERT(m_liveDeviceCount == 0)` in `RHI::destroy()` aborts the process. | **Reproduced in isolation** (`…/logs/matter-runtime-scene-individual.log`). The test tears down RHI from its `PPR_DEFER` while the `MatterRuntime` local (declared before the guard) still owns GPU references; this is a test-teardown lifetime defect, not a lighting regression. Requires an owning-task fix. |
| `game.tests.matter_sand_gpu` | `game/checkpoint_gpu/dug_sand_sustains_sixty_runtime_ticks_on_shared_terrain` fails `readBuffer: 'outData' must not be null` at `Checkpoint.Gpu.Tests.cpp:391`. | **Known red gate** recorded since Task 11 ("checkpoint dug-solids empty-interstice leaf"). |

The GPU-validation matrix is therefore **29/32 after the settlement test
correction**: one known red gate and two test-owned defects remain, both
explained above. The routine matrix was 36/36 at the pre-fix test content; the
changed settlement entry was rerun alone and passes 1/1 (314 s), so no routine
evidence is invalidated. The lighting/viewer validation itself (world-lighting
backend/projection/trace/cascade/fine/material/settlement GPU entries,
reconstruction, picture HDR/spatial, resident particles) passes.


## Execution-log audit

All logs are complete stdout+stderr from the `clang-rel` binary, grouped by
severity/category/message template with numbers/IDs normalized; peak repetition
is per 10 s window. Ordinary and Lab runs were exited by a real window close
(KWin `closeWindow`), not `--exit-after`, so the automation-only auto-exit setup
notice is absent and the shutdown path is the production one.

| Run | Log | Exit | Wall | Records | Warnings | Errors |
| --- | --- | --- | --- | --- | --- | --- |
| Lab sky −1 manual EV0/bloom-off, 1024 seed 42 | `/tmp/opencode/task19/lab/lab-sky-1-manual.log` | 0 | 28.4 s | 277 | 0 | 0 |
| Lab sky 12 manual EV0/bloom-off | `…/lab-sky-12-manual.log` | 0 | 28.4 s | 277 | 0 | 0 |
| Lab sky −1 defaults (auto-exposure+bloom) | `…/lab-sky-1-default.log` | 0 | 28.4 s | 277 | 0 | 0 |
| Lab sky 12 defaults | `…/lab-sky-12-default.log` | 0 | 28.4 s | 277 | 0 | 0 |
| Lab sky −1 probes inspection | `…/lab-sky-1-probes.log` | 0 | 28.4 s | 277 | 0 | 0 |
| Lab sky −1 cascade-3 inspection | `…/lab-sky-1-cascade3.log` | 0 | 28.4 s | 277 | 0 | 0 |
| Ordinary frontier 1024, resize/minimize/restore, close | `/tmp/opencode/task19/ordinary/ordinary-1024.log` | 0 | 30.4 s | 279 | 0 | 0 |
| Ordinary Lighting Lab acceptance (camera/edit/previews/2×New Game) | `…/ordinary-acceptance-1024.log` | 0 | 24.1 s | 1,022 | 0 | 0 |
| Permanent display exercise | `/tmp/opencode/task19/exercise/display-exercise.log` | 3 | 47.3 s | 388 | 0 | 1 (deliberate) |
| Profiling Systems frozen/live | `…/systems-{frozen,live}/launcher.stdout.log`, `profile-capture.log` | 0 | 17.5/19.2 s | 4 engine records each | 0 | 0 |
| Profiling Graphics frozen A/B, live | `…/graphics-{frozen-a,frozen-b,live}/profile-capture.log` + vendor tool stdout | 0 | 10.3 s each | state-machine records only | 0 engine | 0 engine |

Findings and resolutions:

- **E41018 resolved.** The two per-run `warning[E41018]: returning without
  initializing out parameter` Slang diagnostics came from
  `world_lighting_ch.slang:99` (`chFinish` returning false with an uninitialized
  `out CH result`). The out parameter is now zero-initialized on the numeric
  failure path before returning false (the caller reports the numeric category
  and writes containment and never publishes the value). The warning is absent
  from all post-fix runs and from a dedicated cold-shader-cache run
  (`/tmp/opencode/task19/lab/lab-cold-cache.log`, exit 0, 0 warnings/errors,
  capture frame 19579). No diagnostic was filtered or disabled. Diff:

  ```diff
  bool chFinish(CHAccumulator accumulator, out CH result) {
      float total = accumulator.m_weight;
  -   if (!(total > 0.0) || !isfinite(total)) return false;
  +   if (!(total > 0.0) || !isfinite(total)) {
  +       // Numeric failure: the caller reports its category and writes
  +       // containment instead of publishing this value. Keep the out parameter
  +       // initialized on that path so no caller can observe indeterminate data.
  +       for (uint coefficient = 0u; coefficient < 9u; ++coefficient) {
  +           result.m_coefficients[coefficient] = float3(0.0);
  +       }
  +       return false;
  +   }
  ```
- **Vulkan `WARNING-Shader-OutputNotConsumed` (E41018's companion) is gone.**
  The warnings were produced by the retired `vertexFluidComposite` →
  `fragmentFluid/SolidComposite` pipelines (unused vertex output at Location 2).
  Task 11's categorical particle projection retired those pipelines; no
  post-fix run emits `OutputNotConsumed`, so no further shader/pipeline change is
  needed.
- **Message templates and rates.** The highest-rate templates are startup asset
  work, not steady-state chatter: `Image generated mip chain` (134 per world
  load, 26.8/s peak only during load), `decode coerces sRGB source to linear`
  (65/new world), `shader module loaded from file` (25 at startup), and
  `BindlessTextureCache released texture` (268 over the acceptance run's world
  replacements). After the world is published the Lab run emits only 23 records
  in ~22 s (shutdown + one GPU-tick summary); the ordinary run emits 31 in
  ~24 s including the five window-surface resizes, five obsolete-surface packet
  drops, and shutdown. No per-frame, per-particle, retry-loop or
  unchanged-state chatter was observed. Representative lines:

  ```text
  Background#3 🔹 [Image      ] -- generated mip chain {}
  Background#7 🔹 [Image      ] -- decode coerces sRGB source to linear for data usage {"ext": ".png", "forced_linear": true}
  Application  🔹 [Shader     ] -- shader module loaded from file {"name": "mesh_bindless", "path": "…/shaders/mesh_bindless.slang"}
  RenderLane   🔹 [Renderer   ] -- window surface resized {"format": "BGRA8UnormSrgb", "width": 1280, "height": 720}
  RenderLane   🟢 [MatterGpu  ] -- GPU tick timing summary {"valid": true, "samples": 43, "skipped_pending_query": 0, "pending_signal": 0, …}
  ```

- **Display-exercise error is deliberate and labeled.** The single `❌` record
  is `game action rejected {"reason": "display.refresh_hz: Refresh is an
  exclusive fullscreen request only", code 22}` emitted by the
  `invalid_draft_rejected` case, which requested a windowed refresh value to
  prove schema rejection; the case passes. It is fault-injection evidence, not
  an ordinary-execution failure. No other warning/error appears in the
  exercise log; `stale_ack_advanced=false`,
  `emitted_actions_after_close=0`, `quit_confirmed=true`.
- **Profiler runs:** the `clang-rel-profile` build uses `EMPHASIS` logging, so
  Systems application stdout retains four records (run marker, two sparse
  preparation fields whose JSON key is named `error` with value `"Success"`,
  stop marker) and no warning/error. Graphics captures retain the state-machine
  log and the vendor `ngfx` stdout, which ends with its normal teardown text
  `Activity session destroyed, connection error encountered.` and an
  informational `HES is not supported on architecture 'Turing'.`; neither is an
  engine diagnostic. `profile-capture.log` reports `complete` for all five
  captures.

### Task 19 corrections (failed-first evidence retained)

1. `assets/shaders/world_lighting_ch.slang` — the E41018 fix above. Before the
   fix, every ordinary/Lab run emitted two `warning[E41018]` records; after the
   fix, none do (including the cold-cache run).
2. `lib/engine/tests/app/App.WorldLighting.Settlement.Tests.cpp` and the
   `sceneTextureForTest()` comment — the GPU-validation settlement twin failed
   on an invalid MSAA readback (see the matrix table). The test now uses the
   already-compared single-sample history bytes as the committed screen-content
   evidence and keeps the scene pointer as an existence check. Failed and clean
   logs are both retained (`ctest-gpu-validation.log`,
   `ctest-settlement-gpu-rerun.log`).


## Lab / visual evidence for the owner

All six Lab captures use the ordinary CLI at 1024 map, seed 42, 1280×720
window, default pixel scale P=3, engine overview camera (centre 512,512,
vertical span 1056 cells), and were written by `--capture-frame` on graceful
close. Presentation values are recorded from the display exercise's measured
grid at the same W/P: `W=1280×720 → R=427×240, S=214×120, B=428×240`.
The HUD in each captured frame shows `Time 11s`.

| Case | Settings | Captured frame | Artifact |
| --- | --- | --- | --- |
| Sky −1 manual | `--auto_exposure=false --exposure_ev=0 --bloom=false` | 18671 | `/tmp/opencode/task19/lab/lab-sky-1-manual.png` |
| Sky 12 manual | same manual post | 18491 | `…/lab-sky-12-manual.png` |
| Sky −1 defaults | auto-exposure + bloom (committed defaults) | 18648 | `…/lab-sky-1-default.png` |
| Sky 12 defaults | auto-exposure + bloom | 18642 | `…/lab-sky-12-default.png` |
| Sky −1 probes | manual post + `--world-light-diagnostic=probes` | 18551 | `…/lab-sky-1-probes.png` |
| Sky −1 cascade L3 | manual post + `--world-light-diagnostic=cascade --world-light-cascade=3` | 18457 | `…/lab-sky-1-cascade3.png` |
| Ordinary gameplay | frontier 1024, live resize 1280×720 → 1856×1003 after odd/minimize/restore | 19894 | `/tmp/opencode/task19/ordinary/ordinary-1024.png` |
| Display exercise | nine-group permanent matrix | 01–08 PNGs + JSON | `/tmp/opencode/task19/exercise/run1/` |

Lab case coverage at overview scale: the single 1024² frame contains the sealed
dark room, roof aperture, emitter, colored L, three matched doorways
(open/wall/glass), one-cell shell, chunk seams, three basins (sand/water/magma),
the thermal/PBR station, gas and the controlled actor. Manual-vs-default post
and sky −1 vs 12 differ measurably (whole-frame RMSE: manual sky −1 vs 12
0.0050 of range; manual vs default post 0.0233) but the overview scale does not
resolve per-case interiors; the probes and cascade images show the retained
inspection overlays (probe lattice/CH angular mean and the L3 interval atlas with
the second panel's probe grid), matching Task 16's reference captures.

The ordinary run's final capture is at the post-resize framebuffer
`W=1856×1003` (log `final frame captured`), so with the same P=3 the validated
grid is `R=619×335, S=310×168, B=620×336`; the live resize, minimize, restore,
odd/even requests and close were all accepted by the engine with no
warnings or dropped-frame errors (`Renderer window surface resized` ×5,
`dropping packet for obsolete surface generation` ×5).

**Explicit owner gates (not fabricated here):**
- per-case closeups at the specified 1.25 × height span and image-level sky
  −1/12 comparison inside each sealed case (no interactive camera automation is
  available on this host);
- paused-state engine frame from an interactive pause (the frozen profiler run
  below is the engine-level paused state);
- ordinary mouse dig/build and interactive save/reload (the application has no
  interactive checkpoint-load command; `--worldgen-acceptance` exercised the
  production seam dig/edit and two New Games, and the checkpoint API round-trip
  is covered by tests);
- visual acceptance itself. The engine frames above are provided for the owner;
  this report does not self-approve them.

## Display exercise (final binary)

`app.game --exercise-display --exercise-output=/tmp/opencode/task19/exercise/run1`,
exit **3**, 47.1 s: **91 passed / 6 incomplete / 0 failed / 0 unsupported**;
clean shutdown (`quit_confirmed=true`, no post-close emissions, no stale ACK).
The six incomplete cases are the two minimize/restore rounds and their
suspension checks; this XWayland host accepts the production minimize intent but
emits no acknowledged iconified/focus-loss event. Windows/D3D12 remains
unmeasured. Artifacts: `display-exercise.json`, `display-exercise.log`,
`user-settings.toml`, `01-baseline.png` … `08-transactions.png`.
Measured presentation grids (exact, from JSON): P1 `1280×720 → 640×360 →
1280×720`; P2 `640×360 → 320×180 → 640×360`; P3 `427×240 → 214×120 → 428×240`;
P8 `160×90 → 80×45 → 160×90`; P2 odd extents `1919×1079 → 960×540`, and
`1921×1081 → 961×541 → 962×542`.

## Profiling

Fresh captures were made with the Task 18 tooling on a rebuilt
`clang-rel-profile` `app.game` (`cmake --build --preset clang-rel-profile
--target app.game -j16`, exit 0, no warnings). Captures run one at a time with
no other GPU/test workload. Identity records `source_commit=dee01682…` and a
dirty content hash (`c12cbdae…`) covering the Task 19 shader/test corrections
and the user's unrelated stylized asset work; backend `vulkan`, GPU
`NVIDIA GeForce RTX 2080`, driver `620.70.06`, logical/framebuffer
`1280×720`. Profiled executable SHA-256
`9803f72ce9b237c66a75f940ae7e65b943321cc8d73c2d471ca94a4824e8f36a`
(81,099,424 bytes, `clang-rel-profile`); profiler tooling digest `86e5d685…`.
Artifacts live under `/tmp/opencode/task19/profile/`.

| Capture | Duration | Status | Readiness / observed |
| --- | --- | --- | --- |
| Systems frozen | 17.53 s | complete | tick 0, 325 accepted scene frames, 2.003 s active; 434 presents, 39 metrics, `metric_set=null` |
| Systems live | 19.16 s | complete | tick 103, sim frame range 103–807, 328 accepted, 2.002 s active; 372 presents, 43 metrics |
| Graphics frozen A | 10.31 s | complete | tick 0, 359 accepted, 2.002 s active, lighting/screen range 2331–2716 |
| Graphics frozen B | 10.28 s | complete | tick 0, 360 accepted, 2.004 s active, lighting/screen range 2335–2731 |
| Graphics live | 10.33 s | complete | tick 103, sim range 103–114, 328 accepted, 2.006 s active |

Graphics analysis (A/B/live): `overall_status=complete`,
`required_ranges_present=true`, `missing_markers=[]`, `missing_counters=[]`,
**8/8 complete A/B/C frames**, **93 distinct counters / 744 samples**, 0
unavailable metric families. `PBMPM.Tick` is `missing_optional` for the frozen
workloads (no first tick by design) and **present for the live workload**
(`missing_optional_markers=[]`), so the previously unexercised live marker is now
captured. Systems analysis completes on its own checks
(`required_markers_applicable=false`); GPU metric families (`gpu_duration`,
SM occupancy, memory throughput, cache hit rate) are reported unavailable, not
missing, because that capture requests no GPU metric set.

Observed GPU frame times (ms, 8 frames): frozen A
4.737/4.866/4.756/4.830/4.721/4.880/4.804/4.820; frozen B
4.819/4.895/4.786/4.926/4.844/4.864/4.787/4.950; live mean 5.40 with three
~6.2–6.3 ms frames (simulation active). Largest observed marker ranges (frozen A,
median over 8 frames): `WorldLighting.FineCH` 2.003 ms,
`WorldLighting.Cascade.L5` 0.562 ms, `.L4` 0.534 ms, `Hable.Resolve` 0.359 ms,
`.L3` 0.320 ms, `.L2` 0.209 ms, `.L1` 0.177 ms, `scene.stage` 0.159 ms,
`Post.Bloom` 0.097 ms, `Scene.Materials` 0.040 ms (2 ranges/frame),
`Post.AgX` 0.035 ms. These are observed distributions, not thresholds or
convergence evidence.

Replay/comparison: `analyze` run twice on the same raw export produced
byte-identical `analysis.json` (SHA-256
`80fe3ece4f6873ae24141d1f467c09984101a988a488a72b15f205b68a4e86dc`).
`compare` of frozen A vs frozen B reports `comparable=true`,
`conclusion="like-for-like"`, no identity differences;
`baseline/candidate` profiles and `profiler_overhead` caveats are retained in
`comparison/comparison.json`. Raw native exports (`.nsys-rep`, SQLite,
`.ngfx-gputrace`, vendor `BASE_UNLOCKED` text) and normalized analyses are
preserved beside each capture; nothing was overwritten or filtered.

Uncertainties: the Nsight Graphics instrumentation itself is in the measured
frame; no overhead baseline or multipass/replay was measured. Systems captures
are CPU API timelines and do not prove GPU execution. Windows/D3D12 capture is
unmeasured.


## Leak and third-party residual

- LSan default exits 1 on a pre-existing GTK/dbus window/portal allocation set
  (23 allocations, no PPR frames), identical with and without this work; the
  repository NVIDIA/GTK suppression policy is `cmake/sanitizers/nvidia.lsan.supp`.
  No repository-origin leak is open (Task 16 owner-directed audit).
- Third-party additions are attributed in `third_party/README.md`: Filament AgX
  scalar approximation (source-only), Hable MSAA spatial prerequisite
  (source-only), CIE 1931 data (offline LUT input, generated bytes committed),
  and the optional profiling-only Nsight Graphics SDK (not fetched or linked by
  default). No new runtime third-party dependency was added by the port.
- Vendor fixture residual: the captured Nsight Graphics `REPRO_INFO.xls` files
  embed the capture command line (including the artifact directory name); they
  are verbatim vendor exports and were not edited.

## Unrun / unavailable gates

| Gate | Status |
| --- | --- |
| Windows MSVC Dev/Release and D3D12 render/validation/capture | Unavailable on this Linux host — not passed |
| Real minimize/restore/focus acknowledgements | Host-incomplete (XWayland emits no iconified/focus-loss event) |
| Owner per-case closeups and image-level sky comparison | Pending owner interaction |
| Interactive save/reload, mouse dig/build | No application load command; production API/tests only |
| Interactive paused-state frame | Pending owner interaction; profiler frozen run is the engineering pause evidence |
| Real decimal-comma vendor export | Not run — synthetic declared-locale fixture only |
| nsys GPU-metrics sampling, multipass/replay | Not run — capability not exercised |
| Profiler-overhead baseline | Not measured |
| Live-workload `PBMPM.Tick` GPU marker | **Captured in the live Graphics run** (`missing_optional_markers=[]`) |
| GPU-validation matrix fully green | **Unmet** — one stale retired-envelope test (`matter_material_gpu`), one test-teardown device-lifetime abort (`matter_runtime_scene_gpu`), and the known checkpoint dug-sand red gate remain |
| Zero-warning build including vendor std-module diagnostics | **Unmet** — two system libc++ `-Wreserved-module-identifier` warnings per preset |
| Owner visual acceptance | **Pending** |

## Verdict

Spec coverage, YAGNI/naming, ordinary/Lab/display-exercise log cleanliness,
runtime warning resolution and Lab/visual capture evidence are recorded above
with the exact commands, exits and artifacts. Two gates remain open:

1. **Zero-warning builds** are unmet only for the two system libc++
   std-module object-edge diagnostics per preset; project code is warning-free
   under `-Werror`.
2. **GPU-validation matrix** is 29/32 after the settlement test correction, with
   the known checkpoint dug-sand gate and two explained test-owned defects still
   failing (stale liquid-envelope assertion; test teardown device lifetime).

The owner visual gate is open and this document does not self-approve it.
**Owner acceptance pending; Task 19 is not complete and final delivery is not
claimed.**

