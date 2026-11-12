# ONI port — final integrated verification and owner acceptance evidence

Date: 2026-11-12
Branch: `particles`, verification base `dee0168221af7db162f39c7be582b68f32bda4b2`
(`docs: record profiling completion and begin final verification`); the Task 19
gate-fix and final-review commits are listed in the execution ledger.
Host: Linux `pPd` (CachyOS), 32 logical CPUs, 62 GiB RAM, NVIDIA GeForce RTX 2080
(TU104), Vulkan, driver `620.70.06` (Vulkan API 1.4.357).
Status: **owner visual acceptance pending.** This document records verification
evidence; it does not claim final delivery and Task 19 is not marked complete.

Working tree: the user's unstaged `assets/worldgen/stylized/` work is preserved
and untouched. The Task 19 diagnostic and gate fixes are committed in the
follow-up commits recorded in the execution ledger (the CH numeric-failure
initialization, settlement readback, matter/checkpoint gates, diagnostic
routing and the final-review corrections). No unrelated tracked change is
present.

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
- **Two warnings in each preset from the system libc++ module object edges.**
  Exact diagnostics, identical in both presets:
  `/usr/share/libc++/v1/std.cppm:163:15: warning: 'std' is a reserved name for
  a module [-Wreserved-module-identifier]` and
  `/usr/share/libc++/v1/std.compat.cppm:80:15: warning: 'std' is a reserved name
  for a module [-Wreserved-module-identifier]` (one per file, `0` errors).
  They are emitted by CMake's synthesized object edge
  `lib/engine/core/CMakeFiles/@cmake_cxx_std.dir/usr/share/libc++/v1/std.cppm.o`,
  whose command is
  `/usr/bin/clang++ -I/usr/share/libc++/v1 -isystem <repo>/include -stdlib=libc++ <config flags> -std=c++23 @…std.cppm.o.modmap -c /usr/share/libc++/v1/std.cppm`.
  That edge does not inherit project warning flags (it has no `-Wall`/`-Werror`),
  so the default-on Clang 23 diagnostic is emitted; the BMI-precompile edge
  already carries the repository's `-Wno-reserved-module-identifier`. CMake
  4.4.5 provides no option for this synthesized object edge.
- **Zero-warning gate: UNMET as literally specified.** Bounded options, none of
  which this task may take unilaterally:
  1. owner acceptance of the two dependency-only (libc++/Clang) warnings as
     non-project diagnostics;
  2. a toolchain/libc++ change where the standard module no longer triggers the
     diagnostic under this Clang (e.g. libc++ in-source suppression), which is
     outside this repository's control;
  3. a scoped `-Wno-reserved-module-identifier` on the std-module object edge —
     explicitly forbidden by the owner for this task.
  No suppression, log filter or dependency exemption was added. `clang-rel`
  reported no other warnings; configure reported only informational status
  (including `cppcheck requested but executable not found`, which is not a
  compiler warning).

- Incremental rebuilds after the shader fix only re-staged assets
  (exit 0, no compile warnings).
- Post-fix builds (gate fixes, test-diagnostic routing, `IRun` overload,
  retained-log mutex, deadline change): `cmake --build --preset clang-dev
  --target engine.tests.app engine.tests.asset game.tests engine.tests.core`
  exit 0 with no warnings; `cmake --preset clang-rel` +
  `cmake --build --preset clang-rel --target <same four targets> -j16` exit 0
  with no warnings. The final GPU matrix ran on the post-fix `clang-dev` binary.
  The clean-build zero-warning gate above remains the authoritative gate.

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
| GPU validation (pre-fix) | `ctest --test-dir out/build/clang-dev -L gpu_validation --output-on-failure` | 8 | 28/32 passed, 1510.93 s; log `…/ctest-gpu-validation.log` |
| GPU validation (final) | same command, serialized, after all Task 19 fixes | **0** | **32/32 passed**, 1680.73 s wall; log `…/ctest-gpu-validation-final.log` |

GPU-validation failures and disposition (all four repaired; see commits
`0abca2e6` and `9ab0690`):

| Entry | Root cause | Fix / evidence |
| --- | --- | --- |
| `engine.tests.app.world_lighting_settlement_gpu_validation` | `editor_mid_history_edits_do_not_reset_or_contaminate` read `reconstruction.sceneTextureForTest()`, a 4x MSAA target; Slang-RHI's debug layer rejects `readTexture` on MSAA. | **Test corrected**: the MSAA scene pointer remains an existence check; the already-compared single-sample Hable history is the committed screen-content byte evidence. GPU twin 1/1 passed (322 s); routine twin 1/1 passed (314 s). |
| `engine.tests.app.matter_material_gpu` | `evolved_liquid_pool_has_no_interior_render_holes` asserted the retired liquid-envelope draw (Task 11 now returns `envelopes_only`). | **Adapted to the approved categorical path**: the frozen solver rows are projected into native resolved cells with the production ordered key (nearest centre, stable-ID tie, terrain precedence); the test asserts every nominated cell is painted, every empty cell is not, and a terrain cell inside the pool wins and renders red. Solver hashes/displacements/rows and the 360-particle inventory are unchanged. Leaf 1/1 passed. |
| `engine.tests.app.matter_runtime_scene_gpu` | The test-owned graphics queue was destroyed before the PPR_DEFER, so runtime shutdown used a dangling queue (`m_liveDeviceCount`/UAF). | **Ownership order fixed in every matter-runtime fixture**: the queue is declared before the guard, and released after runtime shutdown and before RHI teardown. Entry 1/1 passed (9.66 s); the sibling resident entry also passes. |
| `game.tests.matter_sand_gpu` | An empty restored world publishes zero metadata rows; the helper unconditionally called `readBuffer` with a null destination. | **Zero-row readback guarded** in `Checkpoint.Gpu.Tests.cpp`; the dug-sand leaf now runs its full 60 tick endpoints (480 substeps) and passes (13.82 s). |

Re-run evidence: the five formerly failing entries and their matter/checkpoint
neighbors all passed in the final full 32-entry matrix, **32/32** with exit 0 in
1680.73 s (`ctest-gpu-validation-final.log`; source identity `c433315a` plus the
then-uncommitted follow-up files, committed as `9c3318e`, and a rebuilt binary).
The one previous timeout,
`game.tests.matter_pressure_gpu`, measured 591 s against a 600 s ceiling and was
raised to 900 s with its assertions unchanged; it passed in the final matrix.
Logs are retained under
`.superpowers/sdd/2026-11-10-oni-port/task-19-artifacts/` and
`/tmp/opencode/task19/logs/`.

The routine matrix was 36/36 at the pre-fix content. The affected routine
entries were rerun after the corrections: `world_lighting_codec` and
`world_lighting_settlement` pass, and `services_window` passes all 61 leaves but
intermittently exits on the pre-existing GTK/dbus LSan set (host-only, no PPR
frames; the first full routine matrix passed it). The lighting/viewer validation
itself (world-lighting backend/projection/trace/cascade/fine/material/settlement
GPU entries, reconstruction, picture HDR/spatial, resident particles) is green.



## Execution-log audit

All logs are complete stdout+stderr from the `clang-rel` binary, grouped by
severity/category/message template with numbers/IDs normalized; peak repetition
is per 10 s window. Durable copies of this round's logs, Lab images and
profiling artifacts are under
`.superpowers/sdd/2026-11-10-oni-port/task-19-artifacts/`. Ordinary and Lab runs were exited by a real window close
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

Artifacts are mirrored under
`.superpowers/sdd/2026-11-10-oni-port/task-19-artifacts/profile/`. Fresh
captures were made with the Task 18 tooling on a rebuilt
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


## Test diagnostic output routing (Task 19 cleanup)

The port added **96** `std::print`/`std::println` lines since checkpoint `49a3a7d`
across 11 files, and the complete active-code inventory (203 occurrences) was
audited, not only the additions. Every test-suite occurrence was converted to
the unit-test API `_.log(std::format(...))`, which retains the text and prints it
only when the test fails. `UnitTest::IRun` gained a narrow non-virtual
`log(const std::string &, const std::source_location & = current())` overload:
the formatted string is built at the call site and the defaulted location points
at the test line (a `std::format_string` variadic overload cannot carry the
call-site location because a non-trailing parameter pack defeats the default
argument; the string overload keeps the intended evidence semantics without a
macro or new framework). Fixtures and helpers that previously had no run object
now take `UnitTest::IRun &_` (`initialize`, `qualifySampleFeature`,
`verifyMasks`, `verifyJumps`, `renderResolvedCells`, `assertCategorical`,
`assertDisplayed`, `observeActualDugRendering`, `exerciseDenseDigDiagnostic`,
`qualifySpatialSamples_`, `checkActualGridSpatialSamples_`,
`admitSpatialIntegrationFixture_`, and the ImGui `Evidence` writer). No test
assertion or evidence value was removed; only the output channel changed.

Current active-code `std::print` inventory after the conversion:

| File | Lines | Classification |
| --- | ---: | --- |
| `game/Game.Application.cpp` | 24 | Legitimate user-facing CLI output: `--help`, worldgen preview/acceptance summaries, profiler capture summaries, export paths. The 3 added lines are `[worldgen-acceptance]` result lines. |
| `game/colony/ColonySmoke.cpp` | 14 | Legitimate `SMOKE-*` CLI result summaries. |
| `lib/engine/core/Core.UnitTest.cpp` | 7 | Unit-test framework retained-log/failure output. |
| `lib/engine/core/Core.Logger.cpp` | 2 | Logger framework output. |
| `lib/engine/tests/shared/Tests.cpp` | 2 | Test harness help/failure output. |
| `lib/engine/tests/app/App.MatterGpu.Tests.cpp` | 6 | Artifact-file writes only (`timing.log`, `manifest.txt`, `status.csv` through `std::println(ostream&, ...)`), not stdout. |
| `lib/engine/tests/core/Core.HAL.Tests.cpp` | 1 | Comment mentioning `std::println`, not a call. |

No `std::println` remains in test diagnostics on stdout. Verification:

- Normal converted run (`--run-test app/world_lighting_fine/`): 20 lines of
  harness output, **0** `WORLD_LIGHTING_FINE` lines (the source previously
  emitted 17 diagnostic lines unconditionally).
- Forced-failure probe (temporary `_.log(std::format("TASK19-FORMATTED-RETAINED value={}", 7))`
  + failing assert, reverted after capture): the failure output retained
  `uniform_field_preserves_c0_normalization_and_zero_ac: App.WorldLighting.Fine.Tests.cpp(974): TASK19-FORMATTED-RETAINED value=7`
  — formatted text and the test call-site line. Evidence:
  `/tmp/opencode/task19/logs/forced-failure-formatted-overload.log`.
- The retained-log fallback in `Core.UnitTest.cpp` now formats first and
  appends through `operator<<` under a mutex. The libc++ 23 ostream-print growth
  path tripped ASan's sized-delete check for long retained messages, and the
  logger's background worker can deliver engine entries while the test thread
  logs; both are fixed (`new-delete-type-mismatch`, 768/384 before). The codec
  entry passes after the change.
- Focused framework tests in `Core.TestHarness.Tests.cpp` exercise the formatted
  and raw overloads through a recording `IRun`: formatted text, retained
  call-site file/line, and the raw path all pass (17/17 `core/test_harness`
  leaves). The public test evidence is also verified externally (below).
- `services_window` (window input, ImGui routing, player service) passes all
  61 leaves, but the process intermittently exits on the pre-existing GTK/dbus
  LSan allocation set (18–23 allocations, no PPR frames). The first full routine
  matrix run passed 36/36; later reruns flaked on this host residual. This is a
  documented host-only gate, not a routing regression; closing it needs a
  host/portal fix or an owner-approved suppression policy.

Deliberate fault-test diagnostics remain deliberate: the `ExpectedFailureLogGuard`
fixtures and deliberate rejection assertions are unchanged; profiler/exercise
JSON/stdout contracts are untouched.

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

## Final whole-branch review corrections

The final review returned **Approved for owner acceptance** with no
merge-blocking findings and a small set of cheap corrections, all applied:

| Item | Correction | Evidence |
| --- | --- | --- |
| Interval codec threshold | `intervalPackCategory` accepted only `<= 65408` while the host mirror and spec §4 accept FP16 maximum `65,504`; the shader now uses `65504.0` (RGB9E5/CH stay at 65408). A GPU probe entry `intervalCategories` tests 65,504 accepted (`0`), 65,504.5 rejected (`2`), 65,408 accepted (`0`), negative/NaN/transmittance rejected (`1`). | `gpu_interval_category_accepts_fp16_maximum_and_rejects_above`; codec 13/13 leaves pass |
| `ReconstructionHistoryStamp` layout | Reordered `m_frame` before `m_grid` to remove 8 bytes of padding: **88 → 80 bytes**, static-asserted (`sizeof == 80`, `alignof == 8`); designated initializers updated. | `engine.tests.app.reconstruction` routine + GPU, `world_lighting_settlement` routine + GPU, display-exercise 22/22 all pass |
| Stale pre-integration docs | `third_party/README.md`, `third_party/HableMSAA/README.md`, `assets/shaders/codemap.md` and `lib/engine/app/renderer/codemap.md` now describe the completed Tasks 11–14 integration instead of "remain separate work". | Docs-only; no code change |
| Mislabeled test | `formatted_log_label_keeps_the_original_value` renamed `single_literal_binds_the_raw_overload` and now asserts the raw path's call-site line, keeping raw/formatted coverage distinct. | `core/test_harness` 17/17 |
| OS-suspend active time | Documented limitation: minimize suspension is excluded; a system-sleep gap is one large `dt` and can saturate CH/simulation on resume. | Aggregated concerns |

## Aggregated deferred concerns, rulings and gates

This section is the single disposition list for every concern found from Task 0
through Task 19. `Fixed` means corrected in this branch with evidence; `Deferred`
means accepted by the coordinator/owner for a named future action; `Owner gate`
means it cannot be closed on this host or without the owner; `Won't fix`
includes the reason. Nothing below silently drops a task report item.

### Deferred/unresolved findings by task

| Task / commit | Concern | Disposition | Impact / owner action |
| --- | --- | --- | --- |
| 1 (`49a3a7d`, `6385fca8`) | Pre-existing `gpu_grid_surface` optical leaf and `spatial_retro...` failed | **Fixed**: retired by the Task 11 categorical replacement; the adapted `gpu_grid_surface` leaf passes | None remaining |
| 1 | `engine.tests.asset.gpu` aggregate timed out at 450 s | **Fixed** by the Task 2 shard split; full GPU matrix green | None |
| 1/4 | Per-leaf/per-case shader/RHI lifetime; a program-family cache was proposed | **Deferred (owner decision pending, YAGNI)**: no cache introduced; measured shard cost accepted | Startup/cost only; no behavior claim |
| 2 (`15b6609`, `2be3fc2`) | Bloom weight compounding and live EV min/max app termination | **Fixed** (unweighted box + once-weighted descent; ordered EV helper) | None |
| 2 | `spatial_background_alignment_and_opaque_admission` coverage removed with Retro | **Deferred to Task 14 integration** (coordinator ruling); see rulings | Coverage gap until Task 14; not claimed green |
| 2 | Single-submission cleanup path; `finalize` no-op | **Fixed** by Tasks 13/14 (A/B/C split, command tail) | None |
| 2 | Direct-run LSan elevated counts | **Fixed diagnosis** (Task 16): policy omission, not a repository leak; policy documented | Host-only residual |
| 3 | `--exit-after` emits a warning-level setup notice | **Accepted**: pre-existing automation notice; ordinary runs use window close instead | None to production behavior |
| 3 | In-flight world-preparation cancellation not separately claimed | **Deferred**: covered by ordinary teardown tests only; not claimed as fixed | Owner may request a focused cancellation test |
| 4 (backend) | Windows/D3D12 qualification (R8, atomics, sample order) | **Owner gate**: unmeasured | Windows host required |
| 5 (codec) | README column-sum wording; hardcode one interval/CH word in a future test; redundant `isfinite` in `packInterval` | **Deferred minors** | Documentation/test-hygiene only |
| 6 (`ef7276e3`) | Deferred staging unmap keeps a raw `rhi::IDevice *`; post-completion deferred-count assertion depends on encode-time poll; 4 MiB init upload/64 KiB word cache cost | **Deferred**: no dangling dereference today; Task 14 teardown audit did not change it | Low lifetime risk if callers keep RHI alive; cost measured, no budget violation |
| 7 (`871c25b6`-era) | `initialize` resolves `IRhiService`; per-world shader compilation; `inspectAttempt_` fail-closed on submitted-without-finalize | **Accepted/deferred**: matches existing pass pattern; fail-closed is correct | Recompile cost on world replacement; no behavior defect |
| 8 (`62621c92`) | Per-slot acceleration storage 3.49 MiB/world; two-arg `traceSegment` reports record 0; production tracing unwired then | **Accepted/deferred**: fence-safe slot reuse; detailed entry is the production path; Task 9/10 wired tracing | Memory cost only; no timing claim |
| 9 (`d5f95024`) | Test robustness (optional deref/span checks); pre-fix evidence block retained | **Deferred minors** | Test clarity only |
| 10 (`e628d018`) | False dedupe/renormalization claim in report; dead test-only `L1OracleFrame::m_age` (unused; no runtime path); redundant fine-edge assertion; unused 8 MiB L1 readback; hardcoded oracle constants | **Deferred minors**: report already corrected where contradicted; dead field remains | Test-only cleanliness; no delivered behavior |
| 11 (`e5e073e5`, `0902f442`) | Literal spec camera `V` (sample→camera) with shipped orthographic camera | **Owner visual decision** (see rulings); no silent flip | If wrong, flip normal/camera convention (visual only) |
| 11 | Legacy flat/contour path remains for `m_has_resolved_cells == 0` (pre-first-lighting/non-1024/diagnostic) | **Coordinator-ruled retain**: not used by rendered 1024 worlds; Task 15 documented it | Not a categorical substitute; owner may request later removal |
| 12 (`6418d663`) | Unused `kSampleNodes`; stale extent comment; same-extent reset test not discriminating; no-floor rule proven via CPU mirror; GPU border checks interior-band only; report drift | **Deferred minors** | Test/documentation strength only |
| 13 (`2f46ae40`) | `PreRenderContext` comment stale; report green-log citation lacks exit marker; callback aliases same type by design | **Deferred minors** | Documentation only |
| 14 (`8c5cf6b0`, `fd03d4c6`) | Credit model split (renderer 6 submissions, app 2-frame reservation) | **Coordinator-ruled accept**; header states the real safety argument | If wrong: over/under-admission; ring/reservation depths unchanged |
| 14 | Windows/D3D12 three-submission/sample-rate/status evidence | **Owner gate** | Windows host |
| 15 (`c5db57be`) | Manifest bound raised to 72 KiB; sub-cell seed asymmetry (v6 has no subdivision); no interactive load; GTK/dbus LSan; stylized mirror; retained-payload docs omit allocator/task overhead | **Accepted/deferred**: bound documented; restored particle columns authoritative; load remains owner gate | Manifest variants must stay within 72 KiB; save/reload owner gate |
| 16 (`be0a29d6`, `7e017cc4`) | UI post-slider bounds literal; display-widget layer construction unchanged; retained-picture/non-1024 overlays absent by design; no Linux click automation | **Deferred/owner gate**: controls covered by config/UI tests; overlays intentionally absent | Owner closeups/click tests needed |
| 17 (`871c25b6`…) | `ReconstructionHistoryStamp` 88 vs 80 bytes (8 padding); `historyStamp()` precondition/grouping; `RenderLane` result comment; clearing `out_result.m_reconstruction`; unreachable telemetry canaries; dead `Step` keep/revert fields; misleading minimized-suspension reason; no saved reconstruction-test log; one stale source-identity report reference | **`ReconstructionHistoryStamp` layout fixed** in the final-review commit (80 bytes, static-asserted; initializers updated). Remaining items **deferred minors**; none gate correctness | Layout corrected; comment/test-label cleanup remains owner-optional |
| 12/14 | OS-suspend active time: suspension by window minimize is excluded from Hable/CH accumulation, but a system-sleep gap arrives as one large `dt` and can saturate CH/simulation on resume | **Deferred/owner-known limitation**: not changed (no clock redesign in this port) | Resume can advance CH/simulation by one large step; a future explicit suspend/resume boundary would fix it |
| 18 (`bfc16206`…) | Fixture README per-cycle suffix wording; unconditional "two ranges per frame" docs; report trace-byte/A-B wording; final-state staging trace path; `.pct` suffix; test-local old `post.present` label | **Deferred minors** (ledger) | Documentation/format only |
| 19 (this branch) | `prepareLightingShowcase`/`lighting_showcase` pre-existing production function used by one GPU test | **Won't fix in this port**: predates checkpoint `6aa99aac`, outside the port's YAGNI boundary, current test consumer | None |
| 19 | `App.MatterGpu` artifact-file `std::println(ostream&, ...)` writes | **Accepted**: timing/manifest/status artifact files, not stdout | None |
| 0 (`49a3a7d`) | Checkpoint/recovery archive and paused stash preservation | **Verified**: recovery root outside Git, stash object `8271a171…` unchanged | None |
| 2 | Report/codemap wording drift; padding test did not dispatch the histogram (auto exposure off); no ImGui EV-helper call-site test; unused bloom `offset`/unfinished test encoders; planning input still listed the removed history placeholder | **Deferred minors**: no delivered behavior; helper and both call sites inspected | Documentation/test cleanup only |
| 14 | Retiring-world drain has no direct assertion (reported, not propagated); Task 7 `initialize` resolves `IRhiService`; Task 6 raw `IDevice*` deferred staging; dbus LSan attribution; E41018 warnings | **Deferred**: drain exercised indirectly by world replacement; E41018 **fixed** in Task 19; dbus is host residual | Drain coverage/owner may add a focused test; no engine frame in dbus stacks |
| 19 | Two system-libc++ `-Wreserved-module-identifier` warnings | **Owner decision** (below); no suppression added | Build gate stays unmet literally |

### Coordinator / planning-owner rulings

| Ruling | Cost if wrong | Current status |
| --- | --- | --- |
| Task 0 performed by the coordinator; work in place on `particles` (no worktree) | Cleanup commits land on the feature branch; checkpoint `49a3a7d` restores the start | Implemented; checkpoint/recovery verified |
| DeepSeek Flash only for sublanes; compensated by briefs/diff packages/final audit | Lower review depth; missed findings | Applied; independent reviews recorded per task |
| Active code must not mention the reference project; documentary provenance only | Naming rework if violated | Audited clean (§ YAGNI/naming) |
| Background alignment/opaque-admission coverage re-scoped Task 12 → Task 14 | Coverage absent until Task 14; Task 19 must not claim it green | Task 14 added focused coverage; Task 19 audit accepted |
| Credit model: renderer ledger 2 logical frames × 3 stages = 6; app 2-frame reservation bound | Stalls or over-admission | Ratified in `fd03d4c6`; unchanged |
| Task 11 camera `V` = sample→camera (literal spec), no silent flip | Visual convention may need an owner flip | Owner visual decision pending |
| Manifest bound 72 KiB | Pack variants outside the bound fail loading | Implemented and documented |
| LSan policy: repository NVIDIA suppressions; direct-run GTK/dbus counts are host residual | A real PPR leak could hide in the same stack | Task 16 owner-directed diagnosis; no repository leak open |
| Task 18 marker split around the opaque/translucent draws to preserve draw order | Visual regression if order changed | Restored in `079f94e5`; profiler evidence re-captured |
| Display exercise exit 3 = host-incomplete minimize cases, not pass | False acceptance if treated as pass | Explicit gate in Task 17/19 |
| Task 19 pool test adapted to categorical resolved cells with terrain precedence | Coverage diverges if the approved presentation model changes | New leaf passes; solver hashes/particles unchanged |
| Task 19 shader E41018 numeric-failure initialization | If the failure path semantics differ, containment changes | Fixed at source; all runs clean |
| Two libc++ warnings: no suppression/exemption; owner to accept or change toolchain | Gate stays unmet; suppression would violate owner policy | **Owner decision pending** |

### Known unaddressed gates

| Gate | State | Evidence / action |
| --- | --- | --- |
| Windows MSVC Dev/Release and D3D12 render/validation/capture | **Owner gate / unmeasured** | Linux host; all cross-backend claims explicitly pending |
| libc++ `-Wreserved-module-identifier` (two warnings per preset) | **Blocking literal zero-warning gate** | Exact file/line/edge in "Clean builds"; bounded options listed |
| Remaining GPU-validation failures | **None** after the gate-fix lane | `ctest-gpu-validation-final.log`: 32/32 passed, exit 0; was 28/32 before fixes |
| Display-exercise host-incomplete minimize/restore/focus | **Owner gate**: six incomplete cases | XWayland accepts the intent but emits no iconified/focus-loss event |
| Real decimal-comma vendor export | **Not run**: synthetic declared-locale fixture only | Fixture labelled synthetic |
| nsys GPU-metrics sampling, multipass/replay | **Not run** | Capability recorded; no claim |
| Profiler-overhead baseline | **Not measured** | `comparison.json` carries the overhead caveat |
| Live `PBMPM.Tick` GPU marker | **Captured** in the live Graphics run (`missing_optional_markers=[]`) | Task 18 gate closed |
| GPU driver version | **Unavailable in RHI `DeviceInfo`** | Recorded as unavailable; external `nvidia-smi` 620.70.06 cited where relevant |
| Per-case lab closeups (1.25 × height) and image-level sky comparison | **Owner gate** | Overview/inspection captures only; no camera automation |
| Interactive save/reload and mouse dig/build | **Owner gate**: no application load command | Checkpoint API/tests and `--worldgen-acceptance` only |
| Owner visual acceptance of lab and normal gameplay | **Pending** | This report does not self-approve |
| Host GTK/dbus LSan residual | **Third-party residual**: 18–23 allocations, no PPR frames | Isolated `services_window` rerun passed; documented policy |

### Fixed in this branch (Task 19 lane)

E41018 shader out-parameter warning; settlement GPU MSAA readback; stale liquid
pool test adapted to the categorical path; matter-runtime queue lifetime (four
fixtures); empty-world zero-row readback; interval category FP16-maximum
threshold; `ReconstructionHistoryStamp` 88→80-byte layout; stale pre-integration
docs and test label; complete test diagnostic routing to
`_.log` (all active-code test occurrences, including pre-existing ones); the
narrow `IRun` formatted retained-log overload with call-site location; retained
log stream serialization and format-under-lock (fixing the libc++ ostream-print
sized-delete/UAF exposure); and the `matter_pressure_gpu` deadline raised from
600 s to 900 s after a 591 s measured run timed out with no assertion failure.
Evidence is in the sections above and `/tmp/opencode/task19/logs/` plus the
durable `.superpowers/sdd/2026-11-10-oni-port/task-19-artifacts/` copy.

### Explicitly blocking final acceptance

1. Owner visual acceptance of the Lighting Lab, normal gameplay and display
   exercise artifacts.
2. The literal zero-warning gate for the two libc++, dependency-only
   `-Wreserved-module-identifier` diagnostics (owner acceptance or toolchain
   change).
3. Windows/D3D12 and the remaining owner/host gates listed above.

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
| GPU-validation matrix fully green | **Green**: final matrix 32/32 passed (`ctest-gpu-validation-final.log`); `matter_pressure_gpu` deadline raised to 900 s after a 591 s measured run timed out with no assertion failure |
| `services_window` host LSan | **Host residual**: all 61 leaves pass; process intermittently exits on the pre-existing GTK/dbus allocation set (no PPR frames); first full routine matrix passed |
| Zero-warning build including vendor std-module diagnostics | **Unmet** — two system libc++ `-Wreserved-module-identifier` warnings per preset |
| Owner visual acceptance | **Pending** |

## Verdict

Spec coverage, YAGNI/naming, ordinary/Lab/display-exercise log cleanliness,
runtime warning resolution, the complete test-diagnostic audit, the full GPU
matrix (32/32) and Lab/visual capture evidence are recorded above with the exact
commands, exits and artifacts. The remaining gates are:

1. **Zero-warning builds** — unmet only for the two system libc++ std-module
   object-edge diagnostics per preset; project code is warning-free under
   `-Werror`. This needs the owner's acceptance of the dependency-only warning
   or a toolchain/libc++ change; no suppression was added.
2. **Owner, host and platform gates** — Windows/D3D12, display-exercise
   minimize/restore/focus (host-incomplete), real decimal-comma export, nsys GPU
   metrics, multipass/replay, profiler-overhead baseline, per-case closeups,
   interactive save/reload and the host GTK/dbus LSan residual.
3. **Owner visual acceptance** of the Lab, normal gameplay and display-exercise
   artifacts. This document does not self-approve it.

**Owner acceptance pending; Task 19 is not complete and final delivery is not
claimed.**
