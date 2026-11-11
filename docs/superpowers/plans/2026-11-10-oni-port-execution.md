# SDD ledger — plan: docs/superpowers/plans/2026-11-10-oni-port.md

Plan: [2026-11-10-oni-port.md](2026-11-10-oni-port.md)
Spec: [2026-11-10-oni-port-design.md](../specs/2026-11-10-oni-port-design.md)
Owner approval: the 2026-11-11 launch instruction explicitly approved the current
implementation plan and selected subagent-driven execution.
Execution host: Linux (`pPd`, x86_64), repository `/home/bek/Projects/ppr`,
branch `particles`, Vulkan 1.4.363, NVIDIA RTX 2080 (driver 620.70.06).
Windows/D3D12 is unavailable on this host and remains an unmet gate unless run.

## Setup rulings

- **Ruling:** Task 0 (checkpoint, recovery verification, removal inventory) was
  performed by the coordinator, not a production implementer — it is workspace
  setup and read-only inventory, with no production edit. Cost if wrong: none to
  production behavior; the checkpoint and recovery artifacts are independently
  verified.
- **Ruling:** work continues in place on the existing `particles` feature branch at
  `/home/bek/Projects/ppr`; no new execution worktree is created. The owner did not
  select worktree isolation, the branch is not `main`/`master`, the verified
  checkpoint `49a3a7d` makes recovery explicit, and in-place execution reuses the
  already-configured `out/build/clang-dev` / `clang-rel` trees that validation needs.
  Cost if wrong: cleanup commits land on `particles` instead of a detached worktree;
  the checkpoint restores the starting state.
- **Ruling:** all sublanes use `deepseek/deepseek-flash` only, per the owner's model
  constraint. Review depth is compensated by explicit briefs, diff packages and the
  coordinator's own final audit.
- **Naming:** active code, filenames, symbols, shaders, tests, scripts and profiler
  markers must not mention the reference project. Provenance stays in documentation.

## Task status

| Task | State |
| --- | --- |
| 0 | complete (checkpoint `49a3a7d`, recovery verified, inventory recorded) |
| 1 | complete (commits `14948a7`, `015b4df`, `7c5840b`, `502cf30`; reviews clean; known red legacy gates recorded below) |
| 2 | complete (commits `8835fbb`, `15b6609`, `2be2fc2`; reviews clean; known red gates recorded below; the GPU cost blocker it recorded is resolved by the app GPU cost correction below) |
| 2a | complete (app GPU cost correction: commits `ab37c5ba`, `test: bound matter terrain and runtime GPU entries`; independent review of `ab37c5ba` returned 5 Important + minors, all fixed; records below) |
| 3 | pending — mandatory owner cleanup-acceptance stop; entry conditions and remaining risk recorded in the Task 3 section |
| 4–19 | pending; Task 4 and later must not begin before cleanup acceptance |

## Task 0 — evidence

### Preflight inventory

- Host/root/HEAD detected fresh: Linux `pPd`, `/home/bek/Projects/ppr`,
  branch `particles`, HEAD `88a5e83054e48d631d6bfbfe2ab776c9476815fb`.
- Preflight status: 27 tracked modifications, 0 staged, 42 nonignored untracked
  files (69 changed paths). The former index/worktree distinction is preserved:
  `index.diff` is empty (0 bytes); `worktree.diff` is the full binary/full-index
  worktree diff.
- Durable external recovery root:
  `/home/bek/ppr-recovery/lighting-precleanup/` (outside Git, never committed).
  Artifacts: `manifest.md`, `git-status-before.txt`, `head-before.txt`,
  `branch-before.txt`, `stash-list-before.txt`, `stash-0-object-id.txt`,
  `stash-0-log.txt`, `stash-0.patch`, `index.diff`, `worktree.diff`,
  `tracked-modified-files.txt`, `untracked-files.txt`,
  `worktree-snapshot-20261111T011940Z.tar.gz`, `checkpoint-object-id.txt`,
  `checkpoint-log.txt`, `checkpoint-tree.txt`.
- Inventory digests: `untracked-files.txt`
  `0e32a426c8e1358bc9ebbe2de82739790afc7fb7afee321115e60b4eb99d0ede`;
  `tracked-modified-files.txt`
  `08dd7f79405feb2fda95b6a20d222f20fd1290b92a6f357a3f15bd66ffe96c35`;
  `worktree.diff`
  `77e121452be826fdb4110a9116d549ce221c41b745614e1380606f6c299185ce`;
  snapshot tarball
  `36d18651034247fff0b44513d00b0376f1664fb6173a443859a5cce850d23845`.
- Paused stash preserved by object ID, not position:
  `8271a171148a99fca684a168a8c9acea48d2954c`,
  `task-6b paused: atlas dilation (pending cascade fix)`. Not popped, dropped,
  rewritten or replaced.

### Checkpoint

- `git add -A --dry-run` listed exactly 69 paths (27 modified + 42 untracked); no
  ignored build/cache path was staged.
- `git diff --cached --check` passed.
- Commit `49a3a7d66cbe716a3ac72606716d06c4336e18ed`,
  tree `5ce330fe3d4e2abb44b3d0e15d566057b7a01bd6`,
  message `chore: checkpoint all local work before lighting cleanup`.
- Post-commit `git status --porcelain=v1 --untracked-files=all` is empty.
- Recovery verification: `git archive 49a3a7d` extracted to
  `/tmp/opencode/task0-verify-49a3a7d` (3,574 files); SHA-256 of every formerly
  untracked and formerly modified tracked file matched the recorded manifests with
  zero mismatches. `stash@{0}` still resolves to the original object ID and message.
- No production cleanup, deletion or worktree creation preceded this checkpoint.

## Preflight conflict scan (before Task 1)

| Pair / area | Producer → consumer | Finding | Ruling |
| --- | --- | --- | --- |
| T0 → T1 | checkpoint tree, recovery manifest → deletion boundary | Consistent: all Task 1 paths exist in the checkpoint; untracked `world_light_interval.slang` is now committed and deletable. | None needed. |
| T1 → T2 | temporary cleanup renderer, `App.PostProcess.Tests.cpp`, GridPass/UI/config callers → post extraction | Consistent: T1 keeps palette/Retro temporarily; T2 retires Retro and palette. T1 extracts the nine post leaves before deleting the old suite. | None needed. |
| T1/T2 → T4 | cleanup application acceptance → backend qualification | T3 owner gate precedes T4; no production lighting work before acceptance. | Enforced by the mandatory stop. |
| T4 → T7/T8/T12 | R8 storage, atomics, strides, sample pattern → GPU passes | Consistent: T4 qualifies exactly the features T7/T8/T12 consume and blocks instead of widening formats. | None needed. |
| T5 → T6/T9/T11 | codec types, thermal LUT → publication, source, material | Consistent: `packCell` is shared by resolved cells and metadata; thermal table bytes/sampling are shared. | None needed. |
| T6 → T7 | packed terrain/metadata/palette, shared queue → projection | Consistent: T7 consumes `WorldLightingInputs` declared in T6/T7 interfaces; no second queue. | None needed. |
| T7 → T8/T9/T16 | WorldLightingPass owner, resolved cells, status → acceleration/source/inspection | Consistent: T8/T9 extend the same owner and status contract; T16 reads same world state. | None needed. |
| T8 → T9 | visibility, R8 jump, `traceSegment` → cascades | Consistent: T9 consumes the finite-segment tracer; terminal T=0 is explicitly distinguished from clear T=1. | None needed. |
| T9 → T10 | interval atlas, source, schedule → fine CH | Consistent: T10 reads T9 intervals/source and writes CH in place after source reads. | None needed. |
| T10 → T11/T16 | CH field, `angularMean` → PBR/gas/inspection | Consistent: T11/T16 consume CH and world bindings, not a new representation. | None needed. |
| T12 → T14 | ReconstructionPass, PresentationGrid, ResolvedHdrView → A/B/C integration | Consistent: T14 is the only ordinary-game consumer; cleanup's temporary path is removed there. | None needed. |
| T13 → T14 | command-tail `finalize` seam → stage status copies | Consistent: T13 adds the optional callback; T14 uses it after A/B draws. | None needed. |
| T14 → T15/T16/T17/T18/T19 | integrated ordinary app → lab/inspection/exercise/profiling/verification | Consistent: T15–T18 exercise production paths; T19 audits the complete integration. | None needed. |
| T15 → T16/T17/T18/T19 | Lighting Lab → controls/exercise/profiling/acceptance | Consistent: all run the lab through ordinary loading. | None needed. |
| T16 → T17 | retained controls/diagnostics → display exercise | Consistent: the exercise enumerates the retained controls from §7. | None needed. |
| T17 ↔ T18 | display exercise and profile capture selectors | Both are mutually exclusive CLI selectors; no shared runtime mode. | None needed. |
| T18 → T19 | profiler capture/analysis → final evidence | Consistent: T19 requires actual capture/export/parse evidence. | None needed. |

Per-task self-consistency (files the task creates vs files/tests it later modifies):

| Task | Self-consistency |
| --- | --- |
| 0 | Checkpoint precedes cleanup; ledger/inventory is the only commit; no production edit. Consistent. |
| 1 | Concrete delete list, eight partition removals, mixed-file edits, nine-leaf extraction and post-suite registration agree with the current tree. Inventory additions recorded below. |
| 2 | Presentation values/PostProcessPass declarations agree with T14 consumption; temporary cleanup scene is explicitly removed by T14. Retro leaves listed exist. |
| 3 | Evidence/ledger task plus bounded fixes; owner gate explicit. |
| 4 | Backend test file/shader and sample-pattern reuse agree with T12; no runtime alternative. |
| 5 | Codec/thermal files and assertions agree with T6/T9/T11 consumers; generator/tests listed. |
| 6 | Metadata packing, queue injection and palette publication agree with T5/T7; tests listed. |
| 7 | Projection files/entries and status contract agree with T8/T9. |
| 8 | Two dispatches, 160-byte shared storage, addressing and maximal-jump tests agree with the design. |
| 9 | Source/cascade/sky shaders, interval storage and schedule tests agree with T10. |
| 10 | Fine CH shaders, connector schedule, EMA and in-place contract agree with T9. |
| 11 | Material/BRDF/gas responsibilities agree with T5/T6/T10; unused solid/fluid retirement is explicit. |
| 12 | ReconstructionPass files/tests agree with T14 and T4's sample pattern. |
| 13 | Command-tail seam agrees with existing `std23::function_ref` and T14. |
| 14 | Settlement, retained pictures, active-time and teardown agree with T12/T13. |
| 15 | Lab recipe/material records agree with §6 and ordinary loading. |
| 16 | Diagnostic enum/defaults/overlay agree with §7 and T15. |
| 17 | Exercise state machine, CLI and JSON agree with §7. |
| 18 | Profile capture/CLI/analysis agree with §8. |
| 19 | Final audit/build/log/visual gates agree with the Global Constraints. |

No plan-internal contradiction or plan-mandated review defect was found in the
scan. Findings that emerged only from expanding the inventory are handled in the
next section.

## Expanded removal inventory (Task 1 / Task 2)

The plan's concrete list was verified against the checkpoint tree. The following
additions are required dispositions; they do not change the approved design.

### Production deletions (Task 1, verified present)

`lib/engine/app/renderer/`: `App.Renderer.WorldLightTypes.cppm`,
`App.Renderer.WorldLightField.cppm/.cpp`,
`App.Renderer.WorldLightRadianceAtlas.cppm/.cpp`,
`App.Renderer.WorldLightSource.cppm/.cpp`,
`App.Renderer.WorldLightSourceGpu.cppm/.cpp`,
`App.Renderer.WorldLightProducer.cppm/.cpp`,
`App.Renderer.OpticalGeometry.cppm/.cpp`,
`App.Renderer.Lighting.cppm/.cpp`.

`assets/shaders/`: `world_light_field.slang`, `world_light_trace.slang`,
`world_light_sample.slang`, `world_light_source.slang`,
`world_light_matter.slang`, `world_light_interval.slang` (committed from the
formerly untracked input), `world_light_radiance_atlas.slang`.

### Registrations and imports

- `lib/engine/app/CMakeLists.txt`: eight `.cppm` source lines (43–50) and seven
  `.cpp` lines (109–115); remove exactly with the files.
- `lib/engine/app/App.cppm`: eight `export import` lines (30–37).
- `lib/engine/app/frame/App.Frame.RenderLane.cppm`: imports
  `:renderer.world_light_source` and `:renderer.world_light_types` (11–12).
- `lib/engine/app/renderer/App.Renderer.GridPass.cppm`: imports
  `:renderer.lighting` and `:renderer.world_light_field` (6–7), old owner members.
- `lib/engine/app/App.Application.Editor.cpp`: imports
  `:renderer.world_light_radiance_atlas` (27) and owns field/source/atlas/plan
  members plus `logCapturedWorldLightField` (85–88, 142+). Remove with owners.
- `lib/engine/app/App.Application.Editor.cppm`: remove corresponding declarations.

### Shader dependency and staging dispositions

- `grid_tiles.slang` includes `world_light_sample.slang` (60) and references the
  atlas ambient limit; Task 1 removes the include/ambient path and keeps ordinary
  material/texture drawing. The temporary probe override and commented odd-extent
  checks must be preserved into recovery (already captured in Task 0) and replaced
  deliberately by Task 16, not deleted as drive-by cleanup.
- `world_light_matter.slang`/`world_light_field.slang` include
  `world_light_source.slang`; delete together.
- `world_light_interval.slang` includes `world_light_field.slang`; delete together.
- `world_light_trace.slang`/`world_light_radiance_atlas.slang` include
  `world_light_field.slang`/`world_light_sample.slang`; delete together.
- Runtime loader paths to delete with their owners:
  `WorldLightSourceGpu.cpp` (`world_light_source.slang`,
  `world_light_matter.slang`), `WorldLightField.cpp` (`world_light_trace.slang`),
  `WorldLightRadianceAtlas.cpp` (`world_light_radiance_atlas.slang`).
- Staged copies: `cmake/StageAssets.cmake` copies whole shader directories.
  After source deletion, prune stale copies from
  `out/build/<preset>/game/shaders/`, `out/build/<preset>/lib/engine/tests/app/shaders/`
  and asset-test staged shader directories before running tests. Do not commit
  staged output. `assets/shaders/codemap.md` and `lib/engine/app/renderer/codemap.md`
  are updated in the owning task.

### Game/application/UI/settings

- `game/Game.Config.cppm/.cpp`: `EWorldLighting`/`world_lighting_domain`,
  `EWorldLightDebugView`/`world_light_debug_domain`,
  `WorldLightDisplaySettings` members `m_enabled`, `m_samples_per_frame`,
  `m_level_count`, `m_atlas_ambient`, `m_debug_view`; keep `m_sky_hour` with its
  `-1` black-boundary meaning. Remove `game.world_lighting`,
  `world-light-enabled`, `world-light-samples`, `world-light-levels`,
  `world-light-atlas-ambient`, `world-light-debug` CLI/schema entries.
  `applyPostToRetro`/`configureFieldRetro`/`RetroSettings` coupling is Task 1's
  disconnect and Task 2's rename/removal.
- `game/Game.Application.cpp`: `worldLightFieldActive`, `m_field_requested`,
  `configureWorldLightingCommand_`, `planWorldLight`,
  `buildWorldLightMaterials` call, field budget/ambient/debug assignments,
  `noteLightingPostStartup` world-light argument, `applyPostToRetro` /
  `setRetroSettings` call sites (Retro parts move to Task 2).
- `game/ui/ColonyPanel.cppm/.cpp`: `RetroSettings m_retro`,
  `WorldLightDisplaySettings m_world_light`, `world_light_debug_view` draft,
  lighting/post draft copy-back.
- `lib/engine/app/ui/widgets/App.UI.Widgets.Display.cppm/.cpp`: `RetroWidgetDraft`,
  lighting draft references (`m_world_light_enabled`, ray/level/ambient/debug) and
  palette control. Task 1 removes old lighting draft; Task 2 removes Retro/palette.
- `game/CMakeLists.txt`: remove/adapt the `field_run` CTest entry (line ~352) that
  invokes `--world-lighting=field`; keep the ordinary game/worldgen entries.

### Publication adapter

- `game/colony/SimFramePacket.cppm/.cpp`: remove
  `buildWorldLightMaterials` and its `gridMaterialOpticalCell`/`OpticalKind`
  conversion; preserve packet identity, terrain visuals, dirty/full-refresh
  revisions, stable IDs and solver behavior.
- `lib/engine/app/renderer/App.Renderer.GridPass.cppm/.cpp`: remove
  `gridMaterialOpticalCell`; retain material validation, filtered means, texture
  residency, normal decoding, texture bombing and particle/gas admission.

### Tests

- Delete `lib/engine/tests/app/App.Lighting.Tests.cpp`; remove
  `lightingTests()` from `App.Tests.cpp` and its registration.
- Extract the nine verified leaves (`cpu_histogram_math`,
  `cpu_adaptation_alpha`, `gpu_adaptation_smoke`,
  `gpu_dark_scene_target_ev_raises_exposure`, `cpu_bloom_chain_layout`,
  `gpu_bloom_smoke`, `gpu_bloom_exposure_aware`, `gpu_bloom_bounded_uniform`,
  `gpu_bloom_single_mip_not_halved`) from
  `lib/engine/tests/app/App.WorldLightField.Tests.cpp` into
  `lib/engine/tests/app/App.PostProcess.Tests.cpp`; then delete the old file,
  `world_light_fieldTests()` and the `world_light_field` CTest entry/filter.
- `lib/engine/tests/app/CMakeLists.txt`: remove `app/lighting` from the
  `functional_cpu` filter (line 259) and the `world_light_field` entry (340–341);
  add the GPU-locked `post_process` entry.
- `lib/engine/tests/app/App.GridPass.Tests.cpp`: split optical assertions; remove
  Retro leaves in Task 2.
- `game/Game.Config.Tests.cpp`: delete the three named obsolete leaves; reject
  retired settings; keep post/sky validation and simulation map-size tests.
- `game/ui/Game.UI.Tests.cpp`: delete the world-light gate leaf; adapt post drafts;
  Task 2 deletes the palette leaf.
- `game/Game.Display.Tests.cpp`: adapt the mixed overlay round-trip leaf; Task 2
  removes palette expectations.
- `game/colony/Content.Pack.Tests.cpp`: keep the transport-presentation leaf,
  remove only `gridMaterialOpticalCell`/`OpticalKind` assertions.
- `lib/engine/tests/asset/Asset.Render.Gates.Tests.cpp`: keep the mixed file,
  remove old field setup; Task 2 retires Retro-policy leaves and filters
  `spatial_retro_gpu`/`spatial_retro_gpu_validation`.
- **Inventory addition:** `lib/engine/tests/app/App.MatterGpu.Tests.cpp` uses
  `RetroPass`/`RetroSettings` (2538, 2619, 2639). Task 2 must adapt these to the
  renamed post owner; this is not a Task 1 world-light deletion.
- **Inventory addition:** `game/colony/Checkpoint.Gpu.Tests.cpp` uses
  `RetroPass`/`RetroSettings` (1705, 1772, 1791). Task 2 must adapt it.
- `third_party/FilamentAgX/README.md`, `third_party/FilamentAgX/generate_goldens.py`,
  `third_party/HableMSAA/README.md`, `third_party/README.md`,
  `third_party/SlangRHI/README.md` mention RetroPass in historical/attribution
  documentation or tooling. Task 2 updates renamed references where they describe
  the active owner; provenance stays documented.
- Codemaps: update `assets/shaders/codemap.md`, `lib/engine/app/renderer/codemap.md`,
  `lib/engine/tests/asset/codemap.md`, `game/codemap.md`, `game/ui/codemap.md`
  alongside their owning changes.

### Naming audit baseline

Tracked active code contains one reference-project mention, in
`lib/engine/app/renderer/App.Renderer.WorldLightTypes.cppm:15` (a comment). That
file is deleted in Task 1, so the active-code audit should reach zero after Task 1.
`assets/worldgen/default/PROVENANCE.md` and `docs/` are documentation and retain
provenance/attribution intentionally.

## Commands, backend and evidence

- Host/backend: Linux/Vulkan, NVIDIA RTX 2080; Windows/D3D12 unavailable here.
- Tooling: Python 3.14.9, clang-format 23.1.4, CMake 4.4.5, Ninja 1.13.2.
- Build trees present: `out/build/clang-dev`, `out/build/clang-rel`,
  `out/build/clang-rel-profile` (ignored by Git).
- `scripts/format_cpp.py` and `scripts/check_semantic_style.py` are the format and
  semantic-style gates for every C++ edit batch.

## Task 1 — completion and GPU-suite correction

### Commits and review

- `14948a7` `refactor: disconnect superseded world-lighting integration`.
- `015b4df` `fix: remove orphaned material helper and stale cleanup references`.
- `7c5840b` `test: bound asset GPU correctness shards and retire obsolete performance sampling`.
- `502cf30` `test: preserve live temporal coverage and correct GPU evidence`.
- Task 1 review: 3 Important findings, all fixed; scoped re-review found all
  addressed with no new Critical/Important breakage.
- Timeout/acceleration review: 3 Important findings, all fixed in `502cf30`;
  scoped re-review found all addressed with no new Critical/Important breakage.
- YAGNI: no production abstraction, generic fixture framework or speculative
  cache added. `gridMaterialDiffuseAlbedo` (orphaned) was removed. The only
  retained extra is the test-only `setParticleTimingQueriesForTest` hook, now
  deferred to Task 18; it has no current consumer after the benchmark retirement.

### Timeout diagnosis

- Failing entry: `engine.tests.asset.gpu`, executable
  `out/build/clang-dev/lib/engine/tests/asset/engine.tests.asset`, preset
  `clang-dev` (Debug + ASan), Linux/Vulkan/RTX 2080, `FILTER asset/render`,
  `TIMEOUT 450`, GPU-locked.
- Complete original log: `/tmp/opencode/asset-render.log`; CTest timeout at
  450.17 s. Not a hang: leaves ran serially and accumulated cost. Last completed
  leaf `asset/render/gates/tangent_w_render_proof`; last started
  `grid_solid_particles_share_catalog_textures_and_metric_uvs` at about 400 s.
- Root cause: one correctness entry mixed benchmark-only sampling loops and
  obsolete Retro-policy leaves with live correctness coverage, and every
  `GridPass`/`RetroPass` lifetime eagerly creates its full program/pipeline
  family. ASan stack unwinding amplifies the allocation/submission cost.

### Correction

- Retired three benchmark-only leaves: `grid_material_shading_cost_samples`,
  `picture_temporal_cost_samples`, `grid_liquid_full_resolution_temporal_cost`.
  They had no unique correctness assertion; Task 18 owns replacement
  performance evidence through the real-application profiler.
- Trimmed the timing loop from `grid_liquid_optics_and_viewport_cost` and renamed
  it `grid_liquid_optics_and_viewport_appearance`; appearance assertions remain.
- Split routine correctness into six GPU-locked 450 s responsibility entries:
  `gpu_caches`, `gpu_picture`, `gpu_grid_particles`, `gpu_grid_surface`,
  `gpu_grid_liquid`, `gpu_misc`. Explicit leaf/group token lists are in
  `lib/engine/tests/asset/CMakeLists.txt`; ASan suppression list updated;
  validation entries unchanged.
- Local work removal: collapsed three repeated `GridPass` lifetimes in the liquid
  appearance leaf and two in the solid-particles leaf; shared one `RetroPass`
  across four captures; shared one pass/picture in the slanted-interface leaf;
  removed one fixture-only visibility reset that had no lifetime assertion;
  removed duplicate HDR captures; added `drawOpticalGridOnly_` so discarded-frame
  draws keep encode/submit/wait/retire but skip host readback.
- Two live temporal-policy leaves (`temporal_camera_pan_...`,
  `temporal_layered_background_pan_...`) were restored verbatim after review:
  their production path still exists and Task 2 owns retirement. They run in
  `gpu_picture`.
- Union audit corrected to resolve `gateEditorLeaf()`: 63 leaves total,
  62 routine-owned exactly once, resident-circle leaf owned only by the opt-in
  `resident_particles_gpu` entry, zero duplicates.

### Measured result (same preset/backend/environment, serialized)

| Entry | Result | Wall time | Prior |
| --- | --- | --- | --- |
| `gpu_caches` | pass | 5–7 s | 6.9 s |
| `gpu_picture` (8 leaves incl. restored temporal) | pass | 240 s | 194.4 s (6 leaves) |
| `gpu_grid_particles` | pass | 272–293 s | 320.7 s |
| `gpu_grid_surface` | exit 8 (known red leaf) | 153–167 s | 209.4 s |
| `gpu_grid_liquid` | pass | 187–203 s | 280 s |
| `gpu_misc` | pass | 107–118 s | 119–122 s |
| six-shard sum | | 893–971 s | 1130 s (−14% to −21%) |
| `solid_interface_gpu` | pass | 99 s | 295 s |
| `solid_envelope_gpu` | pass | 180 s | 180 s |
| `spatial_grid_gpu` | pass | 45 s | 45 s |

The restored temporal leaves add 65.1 s inside `gpu_picture`; their coverage is
required until Task 2 removes the production path. No all-entry total is quoted
because the pre-change subset included four entries not re-run identically; those
were re-measured as current behavior only: `spatial_retro_gpu` 54 s exit 8,
`spatial_background_gpu` 99 s, `spatial_grading_gpu` 16 s,
`resident_particles_gpu` 9 s.

Per-leaf improvement examples: liquid appearance 128–129 → 49–50 s; solid
catalog particles 88 → 43–55 s; slanted interface 293–295 → 95–99 s; visibility
104.7 → 78–88 s. Repeat/reordered run reproduced statuses and leaf selection with
an ~8% wall-time spread.

### Known red gates and disposition

- `grid_optical_controls_and_four_layer_parallax` fails at
  `Asset.Render.Gates.Tests.cpp` (changed-pixel refraction assertion). It is
  pre-existing, unchanged by this correction, and owned by Task 11's approved
  categorical-surface replacement. `gpu_grid_surface` therefore exits 8.
- `spatial_retro_matches_reference_and_preserves_lifecycle` still fails on the
  commented odd-extent Retro checks; Task 2 owns the Retro retirement.
- Task 1 acceptance must not claim a green asset GPU suite. These two red gates
  remain visible for Task 2/Task 11 and final Task 19.

### Bounded decision requested (not implemented)

Each test process still pays about 22 s first-pass initialization and about 12 s
per later `GridPass` lifetime; remaining initialization is roughly 40% of the
shard cost. A production program-family lazy initialization or a shader-module
cache would remove most of that, but it is a production API/caching change
outside this cleanup's authority and YAGNI boundary. Owner authorization is
required before such a change. App/post GPU fixture sharing was not started; it
needs its own before/after measurement and is not established by the asset
timings.

### Evidence

- `/tmp/opencode/asset-render.log` — original 450.17 s timeout.
- `/tmp/opencode/asset-timeout-fix/` — per-shard logs, corrected union audit,
  timing probes, repeat/reordered runs, post/correction result tables.
- `.superpowers/sdd/2026-11-10-oni-port/task-1-report.md` — Task 1 implementation
  and fix evidence.
- `.superpowers/sdd/2026-11-10-oni-port/asset-timeout-report.md` — diagnosis,
  correction, coverage disposition and measurements.

## Task 2 follow-up obligations

- **Task 12 (reconstruction coverage):** reintroduce the retired
  `spatial_background_alignment_and_opaque_admission` coverage on
  `ReconstructionPass`: two-resolution layered-background alignment, opaque
  projection admission and retained sample-rate/per-sample source coverage.
  Task 2 removed the old leaf because it drove the retired Retro spatial runtime;
  it is a coverage relocation, not a reduction of the accepted contract.
- **Task 12 (history/exposure invariance):** replace the removed value-only CPU
  placeholder with real evidence that exposure/bloom edits and rejected attempts
  leave Hable screen-history bytes, the previous camera/grid and the committed
  exposure role unchanged. Task 2's `PostProcessPass` now has real rejection and
  padding coverage; only the Task 12 history pair can prove the history half.
- **GPU-suite acceleration (resolved for the app matter suite):** the former
  `engine.tests.app.matter_gpu` exceeded its 300 s Core+GPU-AV deadline. The
  correction is recorded in "App GPU cost correction" below; the timeout was not
  raised, no leaf was skipped and validation was not disabled. `gpu_grid_surface`
  optical controls (Task 11) and the checkpoint dug-solids leaf (Task 11) remain
  known red gates.

## Task 2 — completion: independent post-processing and Retro removal

### Commits and review

- `8835fbb` `refactor: separate post processing from retro reconstruction policy`.
- `15b6609` `fix: correct bloom weighting and post-settings validation`.
- `2be2fc2` `fix: avoid aliasing in single-level bloom descent`.
- Task review: 2 Critical (compounded bloom weights; user-triggerable EV-clamp
  frame exit), 4 Important (value-only rejected-attempt coverage; tautological
  history test; missing padded/R=1/odd GPU evidence; unrecorded background
  coverage obligation) and several Minor findings. All Critical/Important and the
  cheap minors were fixed; the scoped re-review found every finding addressed
  with no new Critical/Important breakage.
- YAGNI: no generic post graph, pass registry, alternate backend or legacy alias.
  `PresentationGrid`/`ResolvedHdrView`/`PostProcessSettings`/`PostProcessPass`
  have current consumers; dead point-sampler/content-submit/presented members
  were removed. The temporary single-sample editor scene image and single
  submission are explicitly Task 12/14 development paths, not runtime modes.

### Delivered behavior

- `engine.app:renderer.presentation`: checked `R=ceil(W/P)`, `S=ceil(R/2)`,
  `B=2*S`, P1..8, positive extents, overflow rejection, zero-framebuffer caller
  suspension; specified post defaults and finite/domain/ordering validation.
- `engine.app:renderer.post_process_pass`: independent exposure/adaptation,
  64-bin fractional histogram, ceil-sized bloom with unweighted raw levels and
  once-only weighted descent, once-only AgX, fixed-P nearest present/crop,
  native UI last, fenced output and retained readers. `finalize` is a documented
  no-op until Task 13; A/B/C split and Hable are Task 12/14.
- Removed `RetroPass` and Retro master/AA/temporal/pixel-mode/palette controls,
  shaders (`retro_present`, `temporal_resolve`, `temporal_2x`), settings,
  persistence, help, UI, CMake filters and test callers. Pixelization P1..8
  default3 and sky hour -1 remain. `spatial_msaa4.slang` remains as the sample
  pattern qualification asset only.
- Assets/codemaps/third-party notices updated. `App.MatterGpu.Tests.cpp` and
  `Checkpoint.Gpu.Tests.cpp` now use the surviving GridPass path.

### Coverage disposition

- Retired with their owner: Retro spatial/lifecycle, two temporal-policy leaves,
  palette UI/config/display expectations, and obsolete grid Retro leaves.
- Retained/adapted: HDR/AgX single encoding, bloom/exposure, native UI,
  rejected-submission/retained-picture/fence contracts, pixelization/extent
  behavior, generic MSAA/sample-pattern, material identity and resource lifetime.
- `spatial_background_alignment_and_opaque_admission` was retired because it
  drove the removed Retro spatial runtime. The committed Task 12 obligation is
  to reintroduce two-resolution layered-background alignment, opaque projection
  admission and retained sample-rate/per-sample source coverage on the new
  reconstruction.
- Removed value-only history placeholder; Task 12 owns real Hable history-byte,
  previous-camera/grid and committed-exposure-role invariance evidence.
- Added real `beginFrame`/`prepare` rejection + `discardUnsubmitted` coverage,
  padded-backing sentinel coverage, and R=1/odd non-square GPU coverage.

### Verification and known gates

- `engine.tests.app.post_process` 16/16; app core entries 5/5; `game.tests`
  pass; affected asset shards and HDR/picture validation entries pass.
- `engine.tests.asset.gpu_grid_surface` remains red on the pre-existing
  `grid_optical_controls_and_four_layer_parallax` (Task 11).
- `game/checkpoint_gpu/actual_dug_solids_...` remains red at the pre-existing
  empty-interstice assertion (Task 11), opt-in.
- **GPU cost blocker resolved:** the former 300 s `engine.tests.app.matter_gpu`
  aggregate is split into bound shards (see "App GPU cost correction" below).
  Evidence for the original failure: `/tmp/opencode/task2-app-matter-gpu.log`.
- Windows/D3D12 unmeasured.

### Deferred minors

- Report/codemap wording drift about the coarsest bloom binding and the replaced
  bounded-uniform assertion.
- `gpu_padding_does_not_reach_metering_or_bloom` does not dispatch the histogram
  path because auto exposure is off; the padding evidence is render/bloom only.
- No ImGui call-site regression test for the EV-ordering helper; the exported
  helper and both live call sites are covered by inspection.
- Unused bloom `offset` declaration and unfinished test encoders.
- The planning input still lists the removed history placeholder among Task 2
  key assertions; this ledger supersedes it.

### Evidence

- `.superpowers/sdd/2026-11-10-oni-port/task-2-report.md` and its fix sections.
- `/tmp/opencode/task2-*.log`, `task2fix-*.log`, `task2fix2-*.log`.

## App GPU cost correction (Task 2 follow-up, pre-Task 3)

### Commits and review

- `ab37c5ba` `test: bound app matter GPU validation cost` — removed two redundant
  solver rebuilds; split the 32-leaf `app/matter_gpu` aggregate into three
  responsibility entries (11/11/10).
- `test: bound matter terrain and runtime GPU entries` (follow-up to the review of
  `ab37c5ba`) — separated the two whole-grid terrain leaves into
  `matter_terrain_gpu`, split `matter_runtime_gpu` into `matter_runtime_gpu` and
  `matter_runtime_scene_gpu`, corrected the report/tables/ledger evidence.
- Independent review of `ab37c5ba` returned 5 Important findings and several
  minors (misstated triggering-run inventory; false lifecycle-coverage claim for
  `isolated_liquid_density…`; unquantified contact-shard worst case; runtime
  evidence gap; stale ledger; plus wording/column corrections). All findings are
  fixed in the follow-up; `ab37c5ba` was not amended.

### Diagnosis

The former `engine.tests.app.matter_gpu` timed out at 300.52 s. In that log 20
leaves completed (234.97 s of printed durations), the 21st
(`terrain_rounded_v_endpoint…`) was in flight for ≥60 s, and 11 leaves never
started. Cumulative cost, not a hang: the unchanged binary re-ran at 231.67 s,
153.04 s and 153.02 s, and the same leaf measured 13.22 s and 127.40 s.

Per-case setup measured with temporary probes (removed before commit): shader
service 0.17–0.26 s, validated device 0.15–0.37 s, queue <1 ms,
`GpuSolver::initialize` 2.05–2.87 s for ten programs, teardown 0.35–0.8 s. Slow
phases are CPU-side (GPU 0–1 % utilisation, 1515–1980 MHz SM), so not GPU clock
throttling.

A group-scoped shader/device/queue owner was implemented and measured, then
rejected: on this host the shared Slang session made every later case's
`solver.slang` module compile ten new entry-point programs at 21.3–22.1 s per
solver (first case 2.094 s; leaf totals 3.78 → 22.29 → 21.54 → 35.14 → 44.31 s)
against a 0.7–1.3 s/device saving. Cases therefore keep private service, device
and queue lifetimes.

### Entries

| entry | leaves | timeout | worst-observed bound | measured wall |
| --- | --- | --- | --- | --- |
| `matter_terrain_gpu` | 2 | 300 s | 206.17 s (68.7 %) | 17 s |
| `matter_contact_gpu` | 9 | 300 s | 61.45 s (20.5 %) | 33 s |
| `matter_material_gpu` | 11 | 300 s | 61.67 s (20.6 %) | 49 s |
| `matter_grid_gpu` | 10 | 300 s | 57.58 s (19.2 %) | 33 s |
| `matter_resident_gpu` | 4 (exact union) | 300 s | — | 14 s |
| `matter_runtime_gpu` | 2 | 180 s | ≈99.5 s (derived) | 35 s |
| `matter_runtime_scene_gpu` | 1 | 180 s | ≈73.1 s (derived) | 25 s |
| `matter_dedicated_gpu` | 1 | 180 s | — | 19 s |
| `matter_fork_gpu` | 6 | 300 s | — | 26 s (105 s in one run) |
| `matter_adapter` | 6 (CPU) | 120 s | — | 5 s |

The four `app/matter_gpu` lists are disjoint explicit tokens and cover exactly the
former 32-leaf union; the runtime lists plus the resident and dedicated leaves
cover the runtime group's 5 leaves exactly once. The lists are explicit, so a new
leaf must be added to exactly one list. The terrain leaves are isolated because
their co-resident worst-observed bound with the contact set was 267.62 s (89 % of
300 s). Deadlines were not raised, no leaf was skipped, no validation was disabled
and no retries were added.

### Verification

Each affected entry ran serially through CTest on `clang-dev` with the original
Core+GPU-AV environment; all exit 0 with the full leaf count
(`after4`/`after5` logs). Filtered single-leaf runs for both terrain leaves and all
three runtime leaves pass; the contact and terrain shards also pass reversed
(`--no-shuffle`) and under two fixed shuffle seeds with every leaf present; an
explicit `ps` capture after the sweep shows no residual `engine.tests.app`
process. Format/style gates and `git diff --check` are clean.

### Resolved and remaining risk

- Resolved: no app matter/terrain/contact/material/grid/runtime entry is expected
  to approach its deadline; the largest worst-observed bound is 68.7 % of 300 s.
- Remaining: host variance is large (one leaf 13.22 s vs 127.40 s; one entry 26 s
  vs 105 s; the aggregate 153 s vs 300.52 s). The shard bounds absorb it.
- Remaining evidence gap: the runtime entry's single 177.55 s occurrence was
  captured as a CTest summary only; eleven later verbose processes (including two
  preceded by the heavy fork entry) measured 59.86–60.67 s, so the runtime split's
  slow-case bound is derived from the observed 2.94× ratio and is recorded as a
  bound, not a measurement.

### Owner decision (pending)

The dominant remaining per-pass cost is production program creation (~2.1–2.9 s ×
29 cases ≈ 61–84 s). Removing it needs either a program-family/shader-module cache
or a separate global-session/compilation-session lifetime in `IShaderService`, so a
group-shared device can be paired with per-case compilation sessions. Both are
production API/lifetime/caching changes and are **not** implemented; the shard
bounds stand until the owner decides.

### Evidence

- `.superpowers/sdd/2026-11-10-oni-port/app-gpu-time-report.md` (corrected).
- `/tmp/opencode/app-gpu-time/` — `tables.txt`, `after2..after5/`, `reorder-*/`,
  `runtime-union/`, `runtime-probe/`, `probe/shared-session-penalty.log`.

## Task 3 — cleanup acceptance (pending owner stop)

State: pending. This section intentionally carries no GPU-cost blocker.

Entry conditions for the owner stop:

- **App GPU cost blocker: resolved.** The former 300 s `app/matter_gpu` aggregate
  no longer exists; its 32 leaves are partitioned across `matter_terrain_gpu`
  (worst observed 206.17 s), `matter_contact_gpu` (61.45 s), `matter_material_gpu`
  (61.67 s) and `matter_grid_gpu` (57.58 s), the resident union keeps its own
  300 s entry (13.00 s), and the runtime entry is split at its unchanged 180 s
  (`matter_runtime_gpu` ≈35 s measured, `matter_runtime_scene_gpu` ≈25 s,
  `matter_dedicated_gpu` 19 s).
- **Pending owner decision, not an acceptance blocker:** the production
  program-family/shader-module cache (see "Owner decision" above). It is an
  optimization; acceptance does not require it, but the residual per-pass setup
  cost stays until it is decided.
- **Known red gates remain owned by Task 11:** `engine.tests.asset.gpu_grid_surface`
  (`grid_optical_controls_and_four_layer_parallax`) and the checkpoint dug-solids
  leaf (`actual_dug_solids_...`). Acceptance must not claim a green GPU matrix
  while those are red.
- **Unmeasured:** Windows/D3D12 build and GPU validation.
- **Not started by this correction:** Task 3 owner acceptance itself, and Task 4
  and later.
