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
| 2a | complete (app GPU cost correction: commits `ab37c5ba`, `3c3b9963`; independent review of `ab37c5ba` returned 5 Important + minors, all fixed; records below) |
| 3 | complete — owner accepted the cleaned application on 2026-11-11 |
| 4 | complete (commit `317d9e8`; independent review approved; D3D12 pending) |
| 5 | complete (commit `2b94778`; independent review approved; D3D12 pending) |
| 6 | complete (commits `ef7276e3`, `02047b7d`, `98136a0e`, `323c6761`; reviews clean; 2 deferred minors) |
| 7 | complete (commit `59cd4c34`; Linux/Vulkan verified; D3D12 pending) |
| 8 | complete (commit `62621c92`; independent review approved; D3D12 pending) |
| 9 | complete (commits `d5f95024`, `563c73a8`; reviews clean; Task 10 handoffs recorded) |
| 10 | complete (commits `e628d018`, `46e021ae`; reviews clean; Task 11 handoffs recorded) |
| 11 | complete (commits `e5e073e5`, `58a21d8a`, `0902f442`, `80c719c7`; reviews clean; camera-side visual gate for Task 19) |
| 12 | complete (commits `6418d663`, `2e0f7ec5`, `b82877b7`; reviews clean; background coverage re-scoped to Task 14) |
| 13 | complete (commit `2f46ae40`; independent review approved) |
| 14 | complete (commits `8c5cf6b0`, `fd03d4c6`; reviews clean; credit ruling recorded) |
| 15 | complete (commits `c5db57be` and the review-fix follow-up; review issues fixed; Task 16 fallback removal recorded) |
| 16–19 | pending |

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
  `ab37c5ba`, commit `3c3b9963`) — separated the two whole-grid terrain leaves into
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

### Graceful-exit defect — closed-session teardown cancellation (fixed)

Reproduced at `86d32920` in an **auto-exit during loading (pre-load
teardown)** ordinary launch: the app exited 0 but logged two error-severity
records during shutdown, a `SessionDeliveryLog` `std::errc::operation_canceled`
from `SessionService::post` and a `Demo` `retainInputError_` failure. Pre-fix
evidence: `/tmp/opencode/task3/ordinary-1281x721.log` lines 463/465; that log
has no `new game published`, `GridPass` shutdown reports `staged_tiles: 0`, and
its screenshot (`/tmp/opencode/task3/ordinary-1281x721.png`) shows the loading
modal. This is not a post-load teardown reproduction.

Diagnosis: `stopControls()`/`close()` close admission, while the lane and
world-completion hooks still deliver their final acknowledgments during
teardown. `post` rejected those through `PPR_RETURN_ERROR_ON_FAIL`, and the
game routed the returned cancellation into `retainInputError_`. Acknowledgments
remain admitted between `stopControls()` and `close()`; cancellation for them
applies after `close()`.

Policy: closed-session cancellation during teardown is expected, not an input
failure. `post` and `deferTool` still return `std::errc::operation_canceled`
(unchanged code and admission semantics) through a documented explicit
`return` with no error diagnostic. `Game.Application.cpp` adds
`postSessionDelivery_`/`deferSessionTool_` helpers in the class's `protected:`
region that ignore only `operation_canceled` and forward every other error to
`retainInputError_`; `retainInputError_` itself is unchanged. All
`retainInputError_(post/deferTool(...))` call sites use the helpers.
`deferSessionTool_`'s closed branch is currently unreachable in production
teardown; it is harmless symmetry, not a required path.

Evidence: `game.tests` and `app.game` build clean; the
`game/session_service/` group passes 13/13 leaves (9.50 s), including
`terminal_control_stop_retains_acknowledgments_until_receiver_close`. Repeating
the originally failing pre-load shape with the fixed binary (auto-exit 70 s,
1281x721, capture frame) exited 0 with 0 `❌`, 0 `FAILED`, and 0
session-delivery/input-failure records; only the auto-exit setup notice
remains. Log `/tmp/opencode/task3/preload-exit-after-fix.log`, screenshot
`/tmp/opencode/task3/preload-exit-after-fix.png`. A separate post-load shape
(auto-exit 130 s) also exited 0 with 0 of those records and 6 pre-existing
Vulkan `WARNING-Shader-OutputNotConsumed` records owned by Task 11; that
warning's pre-existing occurrence is cited from
`/tmp/opencode/task3/worldgen-acceptance.log` (10 occurrences, pre-commit),
because the pre-fix ordinary log exited before emitting it. Logs
`/tmp/opencode/task3/ordinary-clean-exit.log`, screenshot
`/tmp/opencode/task3/ordinary-clean-exit.png`. Exit status 0 was captured by the
invoking shell; the logs are not exit-status artifacts. Commits
`6385fca8 fix: treat closed-session teardown cancellation as expected` and
`docs: correct graceful-exit reproduction and evidence`.

Not claimed: the in-flight world-preparation cancellation behavior is not
separately claimed as fixed (the pre-load run shows no such errors in its log,
but this does not generalize beyond it), and no Task 3 acceptance or
Windows/D3D12 result is claimed. Full report:
`.superpowers/sdd/2026-11-10-oni-port/task-3-report.md`.

## Task 3 — cleanup acceptance evidence (awaiting owner decision)

State: evidence collected; **owner acceptance not yet recorded. STOP here.**

### Fix landed during acceptance

- `6385fca8` `fix: treat closed-session teardown cancellation as expected`.
  Late render/world acknowledgments posted after `close()` now return their
  existing `operation_canceled` code without an error log; `TurboLarbin` routes
  session deliveries through helpers that ignore only that expected closed-session
  cancellation and forward every other error to the existing input-error path.
  `2ff8d944` corrects the evidence labels and records the pre-load re-run.
- The separate in-flight world-preparation cancellation is not separately claimed;
  the observed pre-load post-fix run is clean.

### Build and regression evidence

- `cmake --preset clang-dev` then the affected targets built cleanly in Task 1/2.
- `game/session_service/` 13/13 leaves pass (9.50 s), including
  `terminal_control_stop_retains_acknowledgments_until_receiver_close`.
- Regression closure:
  `ctest --test-dir out/build/clang-dev -R '^(game.tests|engine.tests.app.(functional_cpu|functional_gpu|display_window|post_process|frame_fence))$' -j1`
  → 6/6 pass, 212.23 s (game.tests 123.74 s; post_process 7.04 s).
- Task 2 evidence remains applicable: post suite 16/16, app core entries 5/5,
  affected asset shards and HDR/picture validation entries pass.

### Ordinary application evidence

All runs used the isolated `/tmp/opencode/task3/base.toml` overlay and the
repository sanitizer policy. GPU/window runs were serialized.

| Run | Result | Evidence |
| --- | --- | --- |
| `--worldgen-acceptance` (camera, seam edit, previews, two New Games) | PASS, exit 0; 0 engine errors; 10 pre-existing validation warnings | `/tmp/opencode/task3/worldgen-acceptance.log`, `worldgen-acceptance.png` (1280x720) |
| Post-load ordinary exit, 1281x721 | exit 0; 0 `❌`/`FAILED`; world published 141.9 s; exit 167.1 s | `ordinary-clean-exit.log`, `ordinary-clean-exit.png` (1281x721) |
| Pre-load auto-exit (original failing shape), 1281x721 | exit 0; 0 `❌`/`FAILED`/session-cancellation records | `preload-exit-after-fix.log`, `preload-exit-after-fix.png` |
| Live resize/minimize/restore, then post-load exit | exit 0; 0 `❌`/`FAILED`; window changed 1281x721 → 1856x1003; KWin odd/even/minimize/restore requests acknowledged | `resize-restore-final.log`, `resize-restore-final.png` (1856x1003) |

The application renders the seeded 1024 world with terrain materials/textures,
particles, UI and the retained post path. The deterministic world fingerprint is
stable across the acceptance and ordinary runs.

### Remaining warnings and unverified checks

- 6–10 `WARNING-Shader-OutputNotConsumed` Vulkan validation messages from the
  pre-existing old fluid/solid/particle composite pipelines. They predate this
  task and are owned by Task 11's categorical-surface replacement/Task 19 log
  audit. They are retained raw, not filtered.
- LSan is clean only with the repository NVIDIA suppression policy
  (`cmake/sanitizers/nvidia.lsan.supp`). Without it, driver/DBus global leaks are
  reported; that is the documented external-driver policy, not an engine fix.
- Windows/D3D12 build, validation, sample pattern and presentation are unmeasured.
- Manual owner checks not automatable here: Display Apply/Keep/Revert and timeout,
  per-control post edits, and ordinary mouse dig/build. `--worldgen-acceptance`
  automates the seam edit, camera/preview and two New Games; the rest must be
  observed by the owner.
- Known red test gates remain owned by Task 11: `gpu_grid_surface`
  (`grid_optical_controls_and_four_layer_parallax`) and the checkpoint
  dug-solids empty-interstice leaf. `matter_runtime`/`matter_gpu` deadlines are
  bounded by the recorded shards; the production program-cache decision remains
  pending and is not an acceptance blocker.

### Owner gate

The cleaned application and the evidence above are ready for the owner's visual
and operational acceptance. No Task 4 work has begun.

### Owner acceptance recorded

- **2026-11-11:** owner explicitly accepted the cleaned application with "yes"
  after reviewing the Task 3 evidence and images. The mandatory cleanup gate is
  passed. Task 4 may begin.
- Acceptance covers the intermediate cleanup application, not the final lighting
  visuals or the remaining Task 11/Task 19 red gates.

## Task 4 — backend qualification complete

- Commit `317d9e8` `test: qualify world-lighting storage and Hable sample contracts`.
- Independent review: approved; no Critical/Important findings. YAGNI pass
  (test-only qualification shader/tests, no runtime alternative, no widened jump
  format, no sample remap, no fallback).
- Verified on Vulkan/RTX 2080 with real shaders, pipelines and fenced readback:
  R8_UINT 1024² `{0,1,2,4,8,16,32}`; 32-bit atomic-min invariance under
  forward/reversed/permuted input; reflected strides 4/8/32 and metadata offsets
  0/4/8/12/16/20/24/28; exact 4x sample positions/order; per-sample
  cell-boundary colors; RGBA16F 4x loads; ordered history UAV/SRV; status-copy
  minima 2/3 with zero reserved words.
- Registration: `app/world_lighting_backend/` routine + GPU-validation entries
  pass; exact asset sample-pattern leaf strengthened with exact per-sample
  positions and passes its GPU-validation entry.
- **Task 6 handoff:** the backend test's C++ metadata mirror currently models
  today's `m_material` word. Task 6 must rename it to `m_material_temperature`
  and update the mirror/static assertions (or import and assert the production
  `GridParticleMetadataRow`) while preserving the 32-byte row and offsets
  0/4/8/12/16/20/24/28. The shader mirror and by-name offset table already force
  the update.
- **Deferred minors:** report says 7/7 leaves vs 8/8 group nodes; class-bit
  comment phrasing; SPIR-V disassembly claim lacks a retained artifact;
  duplicated test-local half decoder. None affects the evidence.
- **Unavailable gate:** Windows/D3D12 build, shader/pipeline execution,
  validation layers and sample order are unmeasured and reported pending.

## Task 5 — packed codecs and thermal data complete

- Commit `2b94778` `feat: define packed world-lighting records and calibrated thermal lookup`.
- Independent review: approved; no Critical/Important findings. YAGNI pass
  (shared codec/schema layer, include-only Slang mirrors, no speculative loader
  or runtime alternative).
- Verified: exact FP16/RGB9E5/signed-CH codecs and `packCell`; `makeSourceMaterial`
  mapping/validation; `lastTraceFrame`; CIE CSV SHA-256 and CC BY-SA attribution;
  reproducible 2,048-row/32,768-byte thermal table; `Y(6500)=1`; every finite
  nonnegative FP16 Kelvin through the real GPU table with max relative error
  `0.000261936` (bound 0.001); RGB9E5 error reported separately; GPU reflection,
  compute pipeline and CPU/GPU decoded agreement.
- **Task 6 handoffs:**
  - `makeSourceMaterial` takes `texture_mean`; the missing-albedo white default
    and the roughness/opacity/eta/texture-scale visible-scalar admission belong
    to the Task 6 publication adapter. Test and document them there.
  - `packCell` is the shared material16+half Kelvin encoding for metadata and
    resolved cells; Task 6 renames the metadata word and updates the Task 4 C++
    mirror/static assertions.
  - The derived thermal table is a CC BY-SA 4.0 derivative; retain the
    share-alike/derived-work statement with the vendored/generated data (final
    documentation audit can complete it).
- **Deferred minors:** README column-sum claim vs tested values; hardcode one
  full interval/CH word in a future codec test to pin the intra-word byte map;
  redundant `isfinite` check in `packInterval`. None affects delivered behavior.
- **Unavailable gate:** Windows/D3D12 unmeasured.

## Task 6 — coherent temperature publication complete

- Commits: `ef7276e3` (publication/queue), `02047b7d` (filtered palette +
  fence-safe terrain words), `98136a0e` (device-loss republish + zero initial/
  reset content), `323c6761` (retire terrain staging buffers after readers).
- Review history: initial review found 2 Important (source palette ignored the
  filtered albedo mean; mapped terrain writes had no reader-fence protection)
  plus required test/lifecycle gaps. Fix round 1 addressed them but introduced
  two Important lifecycle defects (device-loss republish; undefined initial/
  reset buffer content); fix round 2 addressed those but recreated the staging
  buffer before in-flight readers retired; fix round 3 deferred the previous
  staging buffer and mapping at `m_world.m_last_reader`, unmaps before release,
  and added a no-wait in-flight reset case. Final scoped re-review: all findings
  addressed, no new Critical/Important breakage.
- Delivered: `m_material_temperature` metadata word (material16+half Kelvin,
  32-byte row/offsets preserved), all consumers masking low material bits,
  temperature validation/rejection before identity commit, terrain packed cells
  through `GridStagedUpload` into a zero-initialized device-local full-domain
  buffer with completion-selected staging and generation-checked acks,
  device-loss/reset republish from the retained CPU cache, explicit Renderer
  graphics-queue injection into Matter (no independent acquisition), filtered
  source-palette derivation from the same computed linear mean, visible-scalar
  admission, once-per-device thermal table with strong refs in
  `WorldLightingInputs`, and real-driver/checkpoint/append/reorder tests.
- **Known gates:** `engine.tests.asset.gpu_grid_surface` remains red only on the
  pre-existing `grid_optical_controls_and_four_layer_parallax` (Task 11);
  Windows/D3D12 and GPU validation execution unmeasured.
- **Deferred minors:**
  - Deferred staging unmap captures a raw `rhi::IDevice *` and runs on the
    confirmed-removal drain, unlike the file's `nullptr`-device convention. No
    dangling dereference today (callers keep RHI alive), but Task 14 teardown/
    lifetime audit must apply the `safe_ptr`/device-survived discipline.
  - Post-completion deferred-count assertion depends on the encode-time fence
    poll; Task 7/14 should assert after a reclaiming draw.
  - Cost notes: 4 MiB full-domain zero image/init upload on each terrain
    (re)creation/reset; 64 KiB CPU word cache per cached chunk; 128 row copies
    per chunk; Vulkan/D3D12 creation-time init behavior differs (D3D12 immediate
    submit/wait). No measured budget violation.
  - One ASan-internal abort in `matter_resident_gpu` did not reproduce; it is
    unrelated by code surface and lacks a captured stack. Preserve as a flake
    until reproduced.

## Task 7 — deterministic categorical cells and asynchronous numeric status complete

- Commit: `59cd4c34` (`feat: resolve categorical world cells with deterministic GPU winners`).
- Delivered: `engine.app:renderer.world_lighting_pass` (`WorldLightingFrame`,
  `WorldLightingPass`, `NumericStatusRecord`) owned by
  `GridPass::WorldResources`; `assets/shaders/world_lighting_projection.slang`
  entries `clearWinners`/`reduceKeys`/`reduceRows`/`resolveCells` over the fixed
  1024-square domain with one shared native-cell coordinate/key helper;
  `assets/shaders/numeric_status.slang` 32-byte record; two 4 MiB row-major
  uint32 scratch grids plus a 4 MiB resolved-cell grid; 32-bit atomic-min class/
  distance key reduction and metadata-rank tie reduction from the same immutable
  committed hot64/origin input; full-domain cell resolve (terrain word, selected
  `packCell` metadata word, or zero); pass-owned 3-slot, 4-record status ring with
  mapped completion-fenced readback, ticket settlement, error propagation and
  teardown inspection.
- Tests: `app/world_lighting_projection/` (12 leaves) — terrain precedence,
  opaque-over-receive-only, near-over-far, exact distance ties with reordered
  numerical source rows (full-grid equality), gas/inactive exclusion, exact
  maximum endpoint vs outside/NaN rejection, 0/1/dense overlap with next-frame
  clearing, invalid/range minima separation and propagation, slot reuse/
  discard/consumption rules, shutdown pending-error reporting, reflected shader
  ABI, and a GridPass ownership/bind integration case.
- Verified on this host: `engine.tests.app` build; `ctest -R
  world_lighting_projection` (routine 8.6 s, GPU-validation twin 8.8 s, Vulkan/
  RTX 2080) both pass; `python scripts/format_cpp.py` check and
  `check_semantic_style.py --files` report only advisory findings (no
  A-STRANDED/B-OP/C-LEADING); `git diff --check` clean.
- **Known gates:** Windows/D3D12 execution unmeasured. Production stage
  orchestration (which calls `prepareWorldLighting`/`finalizeWorldLighting` in a
  real frame and fans settlement into `notifyWorldLightingSubmitted`/
  `collectWorldLightingCompleted`) belongs to the Task 8/14 transport and
  settlement stages; Task 7 proves the owner, contract and device behavior
  through the GridPass integration test. The status copy currently encodes at
  the projection stage end (immediately after `resolveCells`) inside the same
  submission; Task 13's command-tail seam will host the same copy when the
  three-submission split lands.
- **Deferred minors:**
  - `WorldLightingPass::initialize` resolves `IRhiService` for device creation,
    matching `PostProcessPass`; the declared signature remains
    `(IShaderService &, const std::filesystem::path &)`.
  - Shader programs/pipelines compile once per world bundle (first prepare),
    so a world replacement recompiles the four small projection entries. Later
    transport stages will extend the same per-world initialization; review
    whether a GridPass-level immutable program set is warranted then.
  - `inspectAttempt_` fails closed (`state_not_recoverable`) and keeps the slot
    when a submitted attempt never encoded its status copy; the only reachable
    path is a caller that submits without `finalize`.

## Task 7 — categorical projection and numeric status complete

- Commits: `59cd4c34` (projection/status), `77c022d8` (ledger), `1c08f4c5`
  (reject prepare while armed; YAGNI/validation cleanup).
- Review: approved behavior with one Important prepared-attempt/fence
  association defect, fixed in `1c08f4c5`; scoped re-review found all findings
  addressed, no new Critical/Important breakage.
- Delivered: per-world `WorldLightingPass` in `GridPass::WorldResources`; two
  4 MiB uint32 winner grids with shared coordinate/key helper, ordered
  atomic-min keys/rows, stable-ID metadata-row ties, terrain precedence, gas
  exclusion, exact endpoint mapping, invalid/NaN rejection without clamping,
  full-domain `packCell` resolve, no 64-bit atomics/splats/dilation; 32-byte
  numeric status records with two minima, reserved zero words, fixed tickets,
  completion-fenced slot/readback lifecycle, no per-frame host wait, rejected/
  ambiguous settlement; real GPU tests for tie/reorder/terrain/gas/0/1/dense/
  next-frame/endpoint/status/shutdown.
- **Task 8/14 obligations:**
  - Projection validation deliberately requires only terrain + visible materials;
    the later source/transport stage must validate source-palette/thermal
    readiness in its own stage.
  - Every `prepare` must be settled with `notifySubmitted` or
    `discardUnsubmitted`; otherwise the next `prepare` returns
    `operation_not_permitted`.
  - Retiring-world status drain and `inspectAttempt_` fail-closed slot: Task 14
    teardown/settlement audit.
  - `bind` resource-state transitions, including numeric-status SRV/UAV ordering:
    Task 8/14 production binding + D3D12 qualification.
  - `initialize` resolves `IRhiService::get()` internally; revisit when Task 8
    wires production.
- **Known gates:** pre-existing `gpu_grid_surface` optical leaf (Task 11);
  Windows/D3D12 and GPU-validation execution unmeasured on this host.

## Task 8 — visibility, jump table and tracer complete

- Commit `62621c92` `feat: add full-domain jump acceleration and segment tracing`.
- Independent review: approved; no Critical/Important findings. YAGNI accepted
  the per-attempt-slot acceleration storage (3.49 MiB/world, creation-only poison)
  and the detailed tracer entry justified by hit-cell/status evidence.
- Verified: one 21,504-record/172,032-byte uint2 visibility buffer (offsets
  0/16384/20480, pitches 128/64/32); exactly two dispatches with 160-byte shared
  storage, separate level-1 masks, unconditional barriers, no global clear/
  atomics; buildJump priority 0/1/2/4/8/16/32 with R8_UINT; all masks bits
  (including 31/32/63) checked against exact native footprints; SAT maximality
  for all 1,048,576 cells and all seven jump sizes; accelerated-vs-native DDA
  boundary matrix; poison/reuse/queued-submission rebuild; real GPU readbacks;
  dispatch dimensions, stride, R8 format and allocations recorded; SPIR-V
  structure independently confirmed.
- **Task 9/10 handoffs:**
  - Use the detailed tracer form with the owning invocation index for numeric
    status; the two-argument wrapper currently forwards record 0 and is a
    diagnostics footgun. Keep the detailed form for production or document the
    wrapper as diagnostic-free.
  - The finite `kTraceFar` sentinel can turn a pathological overflowing face
    parameter into a clear rather than a numeric failure; close this when the
    tracer gains its production callers.
  - Add a tracer fixture with a receive-only transparent cell in the path and a
    negative interior-grid exact-boundary case; current coverage is indirect.
  - Remove the unused `lighting::visibilityBit` helper or use it in the test.
- **Known gates:** pre-existing `gpu_grid_surface` optical leaf (Task 11);
  Windows/D3D12 unmeasured.

## Task 9 — cached source and L5–L1 transport complete

- Commits: `d5f95024` (transport), `563c73a8` (seam evidence, source buffer
  contracts, status coverage/YAGNI cleanup).
- Review: initial review found three Important issues (seam evidence compared
  the wrong index space; source palette/thermal buffers only null-checked; dead
  exported schedule geometry) plus status/borrow/cleanup gaps. Fix round
  addressed them; scoped re-review found all findings addressed with no new
  Critical/Important breakage.
- Delivered: exact four-frame quarter-probe schedule with uint64 borrow-safe
  frame identity and counter-exhaustion admission rejection; 40 MiB L1–L5
  uint2 interval store with exact atlas addressing, zero L/T cold start and no
  raw L0; L5 four-terminal-sub-sector actual-map-exit tracing with sky closure
  T=0; lower-level clamped four-parent merging with no new parent rays; one
  4 MiB ordered RGB9E5 source from palette/thermal/prior DC with exact Y0
  normalization and pure-metal/transparent/gas/cold-start rules; source→L5→L1
  barriers/status; detailed tracer with owning invocation and nonfinite
  failure. Real GPU tests cover schedule/borrow, source, terminal seam/narrow
  integral, storage/atlas/retention, status range/invalid/containment and
  teardown.
- **Task 10 handoffs:**
  - Task 9 binds the pass-owned zero fine-CH placeholder and proves only the
    pre-update source contract; Task 10 owns the single in-place 512²×32 CH
    field, reconstruction/EMA, and the source-reads-finish-before-fine-writes
    dependency.
  - `lastTraceFrame` remains the planned host schedule interface.
  - The source containment assertion currently reads a cell that starts zero;
    Task 10 should prime it or otherwise make containment discriminating.
  - Production still supplies the default black sky hour until later renderer
    wiring.
- **Deferred minors:** test robustness (optional dereference/span checks);
  report pre-fix evidence block remains alongside the follow-up.
- **Known gates:** pre-existing `gpu_grid_surface` optical leaf (Task 11);
  Windows/D3D12 unmeasured.

## Task 10 — fine CH reconstruction complete

- Commits: `e628d018` (in-place fine CH/connectors/fitting/EMA),
  `46e021ae` (discriminating connector/far-merge/window-once evidence; cleanups).
- Review: production math matched every specified contract; one Important
  evidence gap (nonzero AC/connector geometry/far merge/window-once EMA not
  causally tested) fixed in the follow-up; scoped re-review found all findings
  addressed with no new Critical/Important breakage.
- Delivered: one zero-initialized 512×512×32-byte in-place CH field (8 MiB);
  source-reads-before-fine-writes and fine-writes-before-reader barriers across
  frames; ±4 connector box, clamped L1 bilinear taps with retained trace frames,
  connector-exit basis, `f+0.001v` near trace, `L=Lnear+Tnear*Lfar`,
  `T=Tnear*Tfar`, `2π/16` weights and zero-weight skips; exact CH basis/fitting
  with mean subtraction and once-only Lanczos; decoded FP32 EMA
  `1-exp(-2*dt)` with first-field bypass and injected active time; all 32 bytes
  written, own-record-only access, no history copy; `angularMean(float2)`;
  numeric-status range/invalid containment. Real GPU tests cover uniform/zero
  AC, signed rotated harmonics, connector geometry oracle, clamped edge/far
  merge, window-once EMA, in-place bytes, timing/pause/suspend/rejection,
  barriers and containment.
- **Task 11 handoffs:** use `angularMean` for visible/gas lighting; add the
  sample-rate categorical PBR/BRDF LUT/gas implementation and retire the old
  smooth solid/liquid path.
- **Deferred minors:** report claim that dedupe/renormalization of repeated
  clamped taps would fail the far-merge test is false (unobservable by
  construction); dead `L1OracleFrame::m_age`; redundant fine-edge assertion;
  unused 8 MiB L1 readback in the quadrant test; hardcoded oracle constants.
  Fine CH has no transmittance field; `T=Tnear*Tfar` is covered at the cascade
  level via the shared merge.
- **Known gates:** pre-existing `gpu_grid_surface` optical leaf (Task 11);
  Windows/D3D12 unmeasured.

## Task 11 — sample-rate categorical PBR, shared BRDF LUT and lit gas complete

- Commits: `0902f44` `fix: make categorical view vector consistent and remove dead surface
  fixtures` (re-review follow-up), `58a21d8a` `fix: correct categorical view
  vector and retire legacy surface tests`, on top of `e5e073e5`
  `feat: shade categorical cells with directional PBR and lit gas`.
- Review: six Important findings addressed: engine-correct orthographic
  (`V=-forward`) and perspective (`V=normalize(origin-sample)`) view vectors with
  dedicated NoV/Fresnel probes; the categorical branch moved above the retired
  contour/cell/material work; real categorical texture/albedo/normal/bombing/
  filtered-mean/period-gradient probes; nine retired asset leaf bodies removed;
  `gpu_grid_surface` genuinely covers resolved cells; and a non-zero-CH gas draw
  composition test added. Related fixes restore `albedo.a` in authored opacity,
  remove shader range repair, remove the unused clear seam, use
  `hasPublishedCells()` for normal branching, keep one source/thermal binding
  owner, and release the one-shot BRDF generation program/pipeline.
- Re-review: the orthographic host/shader double negation was removed by naming
  the frame field `m_camera_forward_*` (eye→scene, written as `+forward`) and
  returning `-camera_forward` once in `planeSurfaceToCamera`; an end-to-end
  `GridFrame`→`grid_tiles` gate now renders both orthographic and tilted
  perspective cameras against literal CPU expectations. The default orthographic
  flat-normal camera has `NoV=1e-5` and diffuse term `0.25` for CH mean `0.5`
  (old +forward would give `0.494`); the literal spec V is implemented and the
  visual decision is left to Task 19 owner acceptance (no normal/camera flip).
  The orphaned slanted/five-pixel fixtures were deleted; asset/app leaf counts
  and the adapted categorical `gpu_grid_surface` name were corrected in the
  codemap and report.
- Delivered: exact §4 circular `diffuseIrradiance`/`specularRadiance`; approved
  visible material formula with `kD/kS`, energy compensation, one
  authored+thermal term and no key/ambient floor or specular cap; one immutable
  64x128 RG16F VNDF/Smith table (32 KiB, 1,024 samples/texel,
  `alpha=max(r,0.001)`) per GridPass device; sample-rate resolved-cell material
  selection on the real `grid_tiles` program; transparent premultiplied
  `tint.a*albedo.a*opacity` composition; CH-lit gas with no transport/thermal
  emission; smooth solid/liquid shader families and entries retired.
- Removed `App.Renderer.GridPass.Fluids.cpp`, `App.Renderer.GridPass.Solids.cpp`,
  `grid_liquid_kernel.slang`, `grid_solid_kernel.slang` and
  `grid_fluid_filter.slang`; removed the nine obsolete asset leaf bodies and the
  `gpu_grid_liquid`/`solid_interface_gpu`/`solid_envelope_gpu` entries.
- Evidence: `app/world_lighting_material/` 17 leaves pass routine and GPU
  validation; actual-`grid_tiles` MSAA samples 0/2 vs 1/3 select distinct cells;
  BRDF maximum binary64 error 0.000488; categorical probe verifies albedo,
  normal orientation, bombing UV/weights, filtered-mean normalization and
  period gradients; gas lit/clear composition and non-zero CH mean verified.
  `engine.tests.asset.gpu_grid_surface` now runs the categorical resolved-cell
  boundary/translucency leaf and passes; `gpu_grid_particles`, `gpu_misc`,
  `spatial_grid_gpu`, `resident_particles_gpu` and projection routine/validation
  pass.
- **Task 14/15 obligation:** remove the temporary legacy flat/contour fallback
  once every rendered world publishes resolved cells (including non-1024 edges
  and the first-lighting-publication window). The shipped `gpu_grid_surface`
  gate no longer claims that fallback as categorical coverage.
- **Known gates:** Windows/D3D12 unmeasured.

## Task 11 — categorical PBR, BRDF LUT and gas complete

- Commits: `e5e073e5` (implementation), `58a21d8a` + `12caab70` (first fix
  round: categorical early-out, texture/normal/bombing tests, gas draw test,
  retired surface test bodies, categorical boundary leaf, opacity/saturation/
  binding/LUT cleanup), `0902f442` + `80c719c7` (second fix round: consistent
  view vector, end-to-end GridFrame test, dead fixtures, codemap/counts).
- Review: initial review found six Important issues (perspective/orthographic V,
  legacy work on the categorical hot path, untested texture/normal path,
  compiled retired test bodies, legacy contour lane mislabeled categorical,
  untested non-zero-CH gas). All fixed after two scoped rounds; final re-review
  all findings addressed with no new Critical/Important breakage.
- Delivered: exact CH diffuse/specular filters with zero-XY limits; exact
  visible material formula with Fresnel/energy compensation and no forbidden
  key/ambient/AO/cap; 64×128 RG16F 32 KiB VNDF/Smith BRDF LUT generated once per
  GridPass device lifetime with binary64 reference checks and released after
  boot; sample-rate categorical shading on the real `grid_tiles` MSAA4 program;
  texture bombing/normal orientation/filtered means/period gradients preserved;
  premultiplied receive-only transparency with authored coverage; non-zero-CH
  gas alpha composition before Hable with no relight/ambient; retired smooth
  solid/liquid production path and test bodies; categorical resolved-boundary
  `gpu_grid_surface` leaf green.
- **Owner visual decision for Task 19:** literal spec V (sample→camera) with the
  shipped orthographic camera (origin z≈-0.5, forward +Z) and flat +Z terrain
  gives `NoV≈1e-5` and diffuse ≈0.25 for CH mean 0.5, versus ≈0.494 under the
  old eye→scene convention. No normal/camera flip was applied silently. The
  owner must judge the visual result and decide whether the camera/base-normal
  convention needs a bounded change at final acceptance.
- **Task 14/15 obligation:** the legacy flat/contour `materialSurface` remains
  only as a temporary `m_has_resolved_cells == 0` fallback for non-1024 or
  pre-lighting frames; remove it once all rendered worlds publish resolved cells.
- **Deferred minors:** five-blank-line run in the edited asset test; stale
  `m_surface_to_camera` sentence in the report; CPU-helper section banner;
  report timing freshness.
- **Known gates:** Windows/D3D12 unmeasured.

## Task 12 — always-on temporal Hable reconstruction and review fix

- Implementation commit: `6418d663` `feat: add always-on Hable temporal reconstruction with padded extents`.
- Review-fix commit: `fix: commit reconstruction history only on acceptance`.
- Delivered: `engine.app:renderer.reconstruction_pass` with provisional
  scene/history resources, `assets/shaders/hable_temporal.slang`,
  `PresentationGrid` equality, and `app/reconstruction/` CPU + GPU coverage.
  Extents are `R=ceil(W/P)`, `S=ceil(R/2)`, `B=2*S`; the scene target is Sx by
  Sy RGBA16F 4-sample, the history pair is Bx by By RGBA16F, only R is valid.
- Review findings fixed:
  - **Critical:** `beginFrame` no longer retires the committed pair before
    admission. A different-extent request allocates a tentative pair; only an
    accepted stage-B `notifySubmitted` swaps it in and retires the old pair after
    its readers. A definite discard retires only the tentative pair; a
    capacity-busy `beginFrame` allocates nothing. Tests cover discard and busy
    rejection followed by an accepted original-grid frame that must read the old
    history bytes, not uninitialized memory.
  - **Important:** removed the `1e-4` history-weight floor. The shader and CPU
    oracle skip only exactly zero weights and reject any nonzero out-of-R texel.
    A CPU case proves a 5e-5 padded-texel weight forces current-only.
  - **Important:** the CPU lattice oracle now derives L/R/U/D by a brute-force
    geometric scan for the nearest known node in each direction; it no longer
    copies the shader's residue tables or left/up formulas. A bracket test covers
    positive and negative unknown nodes.
- Task 2 obligations:
  - Hable history bytes, previous camera/grid and committed phase are proved
    unchanged across a rejected attempt and an exposure/bloom/sky-like edit;
    the reconstruction-side acceptance test is
    `gpu_unaccepted_extent_change_preserves_committed_history`. The committed
    exposure role is PostProcessPass-owned and remains covered by the existing
    accepted/rejected `app/post_process` settlement tests; ReconstructionPass has
    no exposure input to reset.
  - Layered background alignment and opaque background admission are **re-scoped
    to Task 14 integration**: ReconstructionPass owns neither the scene/background
    draw path nor its opaque admission; it cannot validly test those draws without
    a new fixture/framework. The reconstruction-side projection alignment across
    P values and retained 4x source coverage are covered here by
    `two_resolution_projection_alignment`, the probe sample dump and the lattice
    phase tests.
- Verification: `app/reconstruction/` 24/24 nodes pass routine; the
  GPU-validation entry passes; `app/post_process` and
  `asset/picture_spatial_gpu_validation` pass; format/style and `git diff --check`
  clean.
- **Known gates:** Windows/D3D12 unmeasured; Task 14 wiring and native
  presentation enlargement remain pending.

## Task 12 — Hable temporal reconstruction complete

- Commits: `6418d663` (ReconstructionPass/Hable/extents/history),
  `2e0f7ec5` (commit history only on acceptance; remove 1e-4 floor; independent
  oracle), `b82877b7` (preserve committed history on same-extent reset discard).
- Review: initial review found one Critical uninitialized-history lifecycle
  defect after unaccepted extent changes, plus a 1e-4 history-weight contract
  deviation, a transliterated "independent" oracle, and an inherited Task 2
  coverage obligation. Fix round addressed them; scoped re-review found all
  Critical/Important findings addressed with no new Critical/Important breakage.
- Delivered: `ReconstructionPass` with exact R/S/B extents, padded/jittered
  projection, procedural Hable lattice/known-node/diamond/bounds, exact boundary
  sample replication, history reprojection/clamp/motion weighting, reset vs
  pan/zoom preserve policy, transactional committed/tentative resource pairs,
  fence-gated reuse, and fail-closed RGBA16F/4x capability admission. Tests cover
  exact grids, padded corners, R=1/odd, lattice/oracle, history/reset matrix,
  rejection/busy extent changes, poison padding and real GPU execution.
- **Coordinator ruling:** background-draw alignment and opaque-projection
  admission coverage is re-scoped from Task 12 to Task 14 integration. The
  reconstruction pass does not own the scene/background draw path; Task 14 must
  add focused two-resolution background alignment/opaque-admission coverage
  before final integration. Cost if wrong: that coverage is absent until
  Task 14; Task 19 must not call it green before then.
- **Deferred minors:** unused `kSampleNodes`; stale extent-deferral comment;
  same-extent reset test does not discriminate the write-index invariant;
  shader no-floor rule proven only through the CPU mirror; GPU border checks
  narrowed to an interior band; report leaf-count/log-citation drift.
- **Unavailable gate:** Windows/D3D12 unmeasured.

## Task 13 — submission-local command-tail seam complete

- Commit `2f46ae40` `feat: support submission-local command-tail encoding`.
- Independent review: approved; no Critical/Important findings. YAGNI clean.
- Delivered: `PostRenderCallback` using the existing `std23::function_ref`
  facility, appended after `prepare` in `Renderer::render`/`renderToTexture`
  with defaults unchanged; invoked after the render-pass encoder ends and before
  timing end/finish/submit on the same borrowed encoder. Tail failure is
  definitely unsubmitted (`kRejected`, no fence advance, immediate credit
  return); attachment writes precede the accepted copy; accepted credit/frontier
  retention proven on a real device with byte-exact attachment/copy comparison.
- **Task 14 handoffs:** use the seam for A's post-draw numeric status copies and
  B's post-output copies; surface passes do not yet carry a tail.
- **Deferred minors:** `PreRenderContext` doc still says "before opening the
  render pass"; report green-log citation lacks an exit marker; "accepted tail
  resources" is credit/frontier retention; the two callback aliases are the same
  type by design.
- **Known gate:** Windows/D3D12 unmeasured.

## Task 14 — A/B/C settlement, retained pictures and teardown complete

- Commits: `8c5cf6b0` `feat: integrate independently settled lighting resolve and
  presentation stages`, plus the review follow-up
  `fix: complete settlement failure matrix and stop C after ambiguity`.
- Review: settlement model approved; three Important findings and minor
  corrections, all addressed in the follow-up:
  1. **Credit-model ratification.** The renderer ledger is
     submission-scoped (`kMaxOutstandingSubmissions = 2 logical frames x 3
     stages = 6`) while the Application keeps its two-logical-frame
     construction-reservation bound. Coordinator ruling: accept this split. The
     header now states the real safety argument (application reservation bound
     plus per-logical-frame last-use rings, not a backend command pool) and
     notes that hosts driving `Renderer` directly are bounded only by the six
     submission credits. Ring depths and the app reservation bound unchanged.
  2. **Failure-matrix coverage.** Added ambiguous **A** and ambiguous **C**
     editor leaves (quarantine, aggregate, no later renderer call, C never
     attempted), a forced numeric-status failure observed by
     `WorldLightingPass::shutdown`, an editor stale-epoch drop leaf (payload
     pending, echo round-trip, no frontier movement), a B history/exposure
     exactly-once leaf across accepted-then-rejected resolution, and one
     combined Review-Focus-1 leaf (B rejected after A while a resize and a world
     replacement are pending: old exposure/history role and retained picture
     hold, A's lighting commit and the scene fence stand, aggregate equals the
     renderer's last accepted value, and a later accepted B moves the role).
  3. **C after ambiguity.** Stage C is skipped whenever a stage reported
     `kSubmitFailed`, and the retained-picture selection is made only when C
     will actually be submitted (no post-pass mutation outside a submission).
- Delivered: `RenderStageResult`/`m_stages` value records and the
  `aggregateAcceptedSubmission` helper; stage A (publication/lighting prepare,
  sample-rate 4x scene, numeric-status tail), stage B (Hable then
  exposure/bloom/AgX, reconstruction `resolved()` after its prepare), and stage
  C (fixed-P present + native UI, retained picture when B is rejected, no
  attempt after ambiguity); suspension-excluding active time at the application
  boundary; the temporary single-sample cleanup scene path removed;
  reconstruction added to the editor lifecycle and teardown; world-candidate
  `begin` carries the world edge (pre-existing publication defect, reproduced on
  the base commit); `WorldLightingPass::bind` attaches numeric status only where
  the program declares it; retiring-world lighting status drained before
  release.
- Tests: `app/world_lighting_settlement/` 17 leaves routine **664 s** and
  GPU-validation twin **669 s** (900 s entries), plus frame_fence, slow_gates,
  services_cpu, projection (+validation), trace, cascade, material, asset
  `gpu_grid_surface`, asset `gpu_misc` and `game.tests`/`matter_restore_gpu`
  green. Normal 1024 app: exit 0, world published/played, capture at 1280x720,
  proven quiescence; only the two pre-existing E41018 shader warnings.
- **Known gates:** Windows/D3D12 unmeasured for the three-submission path,
  sample-rate 4x scene target and numeric-status tail. The
  `m_has_resolved_cells == 0` flat fallback remains only for worlds with no
  published catalog until Task 15's 1024 admission. Live interactive resize is
  unavailable in this environment (covered by the real-window settlement test).
- **Minor follow-ups folded in:** dropped-stage diagnostic distinguishes a
  stage rejection from a non-rendering frame; stale `stampSubmittedPasses_`
  test references updated; the test-only per-stage failure seam is consumed by
  exactly one executed frame (an armed stage the frame never reaches is
  discarded) with a dedicated guard leaf; the inline settlement closure is
  documented as a future multi-stage trap; the OS-suspend `dt` caveat is
  recorded separately from minimized suspension without changing the clock.
- **Deferred minors:** Task 7 `initialize` still resolves `IRhiService`
  internally; Task 6 deferred staging release still captures a raw device on the
  confirmed-removal drain.

## Task 14 — A/B/C integration complete

- Commits: `8c5cf6b0` (A/B/C settlement/retained pictures/teardown),
  `fd03d4c6` (credit text, complete failure matrix, skip C after ambiguity,
  minors).
- Review: approved settlement model with three Important issues (credit-model
  ratification/comment, failure-matrix gaps, C attempted after ambiguity). Fix
  round addressed them; scoped re-review found all findings addressed with no
  new Critical/Important breakage.
- Delivered: value-only `RenderStageResult`/`m_stages` A/B/C with immediate
  per-call settlement/fence capture; accepted-only lighting phase/CH/active-time
  and screen-history/exposure commits; retained-picture C reader fencing;
  ambiguous quarantine/no discard/no retry; C skipped after any ambiguous stage;
  boundary-captured suspension-excluding active time; inverse world/shutdown
  teardown; removal of the temporary cleanup scene path. Fixed two pre-existing
  defects found by the normal-app gate (candidate `m_world_edge` copy; invalid
  `g_numeric_status` bind on terrain).
- **Coordinator ruling — credit model:** the renderer ledger is
  submission-scoped (`kMaxOutstandingSubmissions = 2 logical frames × 3 stages =
  6`); the 2-logical-frame bound remains in `Application` via its reservation
  bound. Safety comes from that reservation bound plus the per-logical-frame
  last-use rings, **not** from the backend command pool. Direct `Renderer` users
  are bounded only by the 6-submission ledger. Reservation bound and ring depths
  were not changed. Cost if wrong: a third queued submission behind a stuck host
  gate has only the Application reservation backstop; revisit if a non-editor
  multi-stage user appears.
- Failure matrix: ambiguous A/C, forced status failure at pass shutdown, editor
  stale-epoch drop/fresh packet, B history/exposure exactly once, and combined
  Review Focus 1 (B rejected after A with resize/world replacement pending;
  aggregate = A's actual; A commit/history role retention).
- Verification: settlement suite 17/17 routine (~664 s) and GPU-validation
  (~669 s) under 900 s entries; frame/lane/run-gate and affected grid/asset/game
  entries pass; normal 1024 app run clean (capture, quiescence, shutdown; only
  two pre-existing E41018 shader warnings).
- **Deferred minors for Task 19 audit:** undocumented `display_window` timeout
  600→900 s bump and unverified "~690 s validation" comment; residual
  ledger-is-2-frames wording in `App.Renderer.cppm`/renderer codemap; OS-suspend
  `dt` caveat beyond minimized suspension; injection seam reset semantics;
  post-pass selection mutation outside a submission not directly observable;
  retiring-world drain reports rather than propagates; Task 7 internal
  `IRhiService` resolution and Task 6 raw-device deferred staging unchanged.
- **Task 15 handoffs:** enforce 1024 rendered-world admission/startup default and
  remove the temporary `m_has_resolved_cells == 0` legacy fallback once all
  rendered worlds publish resolved cells; the lab supplies the ordinary visual
  acceptance scene.
- **Unavailable gate:** Windows/D3D12 unmeasured.

## Task 15 — Lighting Lab review fixes complete

- Commits: `c5db57be` (feature) and the review-fix follow-up
  `fix: make the Lighting Lab receive-only and link the app tests`.
- Review fixes: registered `WorldGen.LightingLab.cpp` in the directly compiled
  app-test colony subset (fixes the `engine.tests.app` link); lab/shared glass
  strength 0 for the engine's receive-only rule; sRGB-authored linear bases;
  rendered checkpoint admission API (`admitRenderedWorldCheckpoint` /
  `restoreRenderedWorldCheckpoint`) with a discriminating lab test; corrected
  manifest-cap evidence and tightened the bound to 72 KiB; paired sky -1/12
  manual-EV0/bloom-off and default-auto/bloom captures; 1024 defaults in the
  editor and workshop state; lab-world identity requires the semantic material
  set; explicit colored-L identity; acceptance seam targets the specified
  `[127,129)×[344,352)` barrier; receive-only/gas/magma/zero-velocity assertions;
  Start-withheld UI coverage.
- **Task 16 handoff:** remove the remaining non-categorical grid path (diagnostic
  overlays, synthetic fixtures, first-publication window, publication failure)
  when the retained inspection views replace the temporary grid probe. All
  rendered 1024 production worlds already publish resolved cells.
- Evidence: `game/lighting_lab/` 5 leaves, full `game.tests`, link/run of
  `engine.tests.app`, sim/checkpoint/worldgen/app-profile entries, four paired
  captures, and `--worldgen-acceptance` PASS with seam target (127,344).
- **Known gates:** Windows/D3D12 unmeasured; no interactive checkpoint load UI
  (API + tests only); per-case closeups/image-level sky comparison and
  interactive save/reload remain owner/Task 19 gates. Default LSan reports the
  pre-existing GTK/dbus 23-allocation leak set on both lab and frontier runs.

## Amendment — persistent compiled-shader cache

- Commits: `c0b8a38b` (feature) and `fix: silence shader-cache misses and
  ignore cache root` (review follow-up). Files: `lib/engine/rhi/RHI.cpp`,
  `.gitignore`, `lib/engine/rhi/codemap.md`, this ledger. Task 15 files and the
  unstaged `assets/worldgen/stylized/` user work are untouched.
- **API evidence:** `rhi::IPersistentCache` (write/query, concurrent-call
  contract) at `out/build/clang-dev/ppr-slang-rhi-msaa/include/slang-rhi.h:4335-4350`;
  `DeviceDesc::persistentShaderCache` at `:3660`; device `ComPtr` addRef at
  `out/cpm_cache/slang-rhi/6e3a/src/device.cpp:432` and `src/device.h:523`;
  key preparation/query-on-miss/write-on-success at `src/shader.cpp:162-227`;
  Slang key covers dependencies + target settings (`slang.h:5528-5535`);
  `slang_createBlob` returns one reference (`slang.h:5903`);
  `getBuildTagString` (`slang.h:4160-4162`); compilation reports
  `isCached`/compile timings (`slang-rhi.h:237-250`, `:4066`).
- **Mechanism:** file-local `PersistentShaderCache final :
  slang_rhi::IPersistentCache` in `RHI.cpp` (no exported alias). Root
  `<cwd>/.ppr-shader-cache/<format-version>/<slang-build-tag>/<device-type>/`;
  entry = lowercase hex of the opaque key + `.bin`; no header/checksum/hash;
  no target-format segment. Build tags filtered to `[A-Za-z0-9._-]`, `.`/`..`
  and >64-character tags omitted. Writes use `<hex>.<pid>.<counter>.tmp` and
  publish by same-directory `rename`; destination-exists after a lost race is
  success. Keys >128 bytes or paths over the platform limit are miss/no-store;
  reads >64 MiB are rejected before reading. Misses use non-throwing
  `file_size` + `ifstream`, so an empty cache emits no error log. A root that
  cannot be created is never installed; a later write failure disables the
  process cache after one info log; first hit/miss log once each. Service owns
  one reference beside the device, sets the desc before `createDevice`, clears
  it in the failure guard, and releases it after the device in `shutdown()`.
  Deliberately unbuilt: module IR cache, pipeline cache, eviction/size
  caps/stats/CLI clear/telemetry, shared sessions/devices, config/env/public
  API, fsync/mutex. Reset is deleting the directory.
- **Cold/warm, direct serial `--no-shuffle` runs on the normal build**
  (clang-dev Debug+ASan, RTX 2080; cold = first run with an empty
  `.ppr-shader-cache`; warm = populated):

  | entry | cold | warm | leaves | `.bin` |
  |---|---|---|---|---|
  | `app/frame_fence` | 6.50 s | 5.57 / 5.55 s | 29/29 | 11 |
  | `app/world_lighting_trace` | 22.14 s | 21.16 / 21.22 s | 9/9 | 11 |
  | `app/world_lighting_settlement` | 32.94 s | 28.63 / 28.54 s | 19/19 | 62 |
  | `app/reconstruction` | 8.28 s | 8.07 / 8.22 s | 25/25 | 4 |
  | `asset gpu_grid_particles` | 6.56 s | 5.05 / 5.16 s | 8/8 | 14 |
  | `asset gpu_misc` | 7.03 s | 5.18 / 5.08 s | 27/27 | 28 |
  | `reconstruction` + `PPR_TEST_GPU_VALIDATION=1` | 11.55 s | 11.18 / 11.30 s | 25/25 | 4 |
  | `settlement` + `PPR_TEST_GPU_VALIDATION=1` | 33.58 s | 29.53 s | 19/19 | 62 |

  Equivalent CTest entries on the normally linked `engine.tests.app` all pass
  cold and warm with identical leaf counts and `.bin` counts, zero `.tmp`
  leftovers and no rewritten entries: frame_fence 6.70→5.59 s, trace
  22.10→21.41 s, settlement 355.84→256.02 s, reconstruction 8.85→8.48 s,
  grid 56.23→22.31 s, misc 82.73→34.45 s, reconstruction validation
  11.87→11.34 s, settlement validation 364.19→264.65 s. Real-window CTest wall
  times swing by 2–6× between identical warm runs (50–100 s observed for the
  same asset shard), so materiality is read from the direct serial table; CTest
  supplies pass/leaf/empty-temp equivalence. Reconstruction benefits least
  (4 shader entries; delta within warm-run variance); the other entries reduce
  materially and repeatably. Keep.
- **Residual front-end cost** (temporary compilation-report instrumentation,
  reverted before commit): `frame_fence` warm serves 38/38 entry points with
  `compile=0 ms`, `create=1.5 ms`, `pipeline=6.8 ms`; the cold device recorded
  938–955 ms of compile (Slang 715–723 ms + downstream 223–229 ms) from 11
  misses (27/38 were already cached in-process). The remaining warm 5.5 s is
  Slang module load/link/specialization, device/pipeline setup and the test's
  GPU work; available instrumentation does not split those (`getCompilerElapsedTime`
  reports 0 ms once artifacts are served; HAL profile scopes are no-ops on
  Linux clang-dev).
- **No-error evidence:** with temporary per-entry severity tracing, a cold
  `frame_fence` run (11 file misses) produced zero cache-related error entries:
  0 `throw exception`/`mapFile`/`failed to map`/`shader cache disabled`
  markers, and the same 4 pre-existing retained error-level entries as the warm
  run (2 Shader E41018 notices, 2 Renderer deliberate-failure lines). The
  previous `io::mapFile` miss path logged a category-Hal `throw exception` at
  error severity on every cold lookup; that path is gone.
- **Known gates:** Windows/D3D12 rename/exists branch unrun; no eviction,
  size accounting or telemetry; default LSan still reports the pre-existing
  GTK/dbus window/portal leak set on windowed entries (identical with and
  without cache use; CTest entries pass).

## Task 15 — Lighting Lab and 1024 admission complete

- Commits: `c5db57be` (lab feature) and `a9066a3a` (review fixes + app-test
  registration).
- Initial review found five Important issues (lab glass transport-opaque;
  sRGB-authored values vs specified linear bases; checkpoint admission not
  wired; visual-acceptance overclaim; manifest-cap evidence/justification)
  plus minors. Fix round addressed them; scoped re-review found all Important
  findings addressed with no new Critical/Important breakage.
- Delivered: ordinary `game.colony.worldgen` `populateLightingLab` through the
  normal prepared-world factory; exact §6 rooms/doors/L/one-cell/seam/basin/
  thermal/PBR/floor/actor geometry and material keys; 3×3072 basin seeds plus
  64 oxygen with stable IDs/temperatures; deterministic content across seeds;
  normal checkpoint round-trip; 1024 startup/admission with 2048/4096 rejected
  before GPU preparation; guarded rendered-checkpoint admission API; 72 KiB
  manifest cap (schema/keys unchanged); paired sky -1/12 manual-EV0/bloom-off
  and default captures; `engine.tests.app` now links `WorldGen.LightingLab.cpp`.
- Lab glass is now receive-only (strength 0.0) including the shared default-pack
  glass record; authored sRGB bytes decode to the specified linear bases;
  tests assert decoded values.
- **Remaining owner/Task 19 gates:** per-case closeups, image-level sky
  comparison, interactive save/reload UI (no production load UI), Windows/D3D12,
  and the Task 11 camera-side visual convention. The stylized mirror glass still
  authors strength 1.0 (uncommitted user work; note for the owner). Checkpoint v6
  subdivision asymmetry and `summarizeLab` retained-memory under-report are
  documented minors.
- **Task 16 handoff:** replace the temporary diagnostic/contour fallback with
  the approved inspection overlay; remove obsolete debug controls.

## Bounded amendment — persistent compiled-shader cache

- Commits: `c0b8a38b` `perf: cache compiled shader artifacts on disk`, `9d00c70`
  `fix: silence shader-cache misses and ignore cache root`.
- Mechanism: file-local `PersistentShaderCache : rhi::IPersistentCache` wired via
  `DeviceDesc::persistentShaderCache`; slang-rhi hashes/looks up/writes entry
  points (no module IR or pipeline cache). Root
  `<cwd>/.ppr-shader-cache/<format-version>/<slang-build-tag>/<device-type>/`;
  lowercase-hex key files; temp+rename; 128-byte key/64 MiB read caps; silent
  misses; one-time disable info log. `.ppr-shader-cache/` is git-ignored.
- Independent review found the miss path used throwing `io::mapFile`/`PPR_THROW`
  and logged errors; fixed with `file_size`+`ifstream`+`slang_createBlob`,
  temp names including the key hex, build-tag sanitization, codemap/ledger and
  `.gitignore`. Scoped re-review: all findings addressed, no new
  Critical/Important breakage; YAGNI clean.
- Cold (empty directory) / warm measurement on the normally linked
  `engine.tests.app`, serial:
  `frame_fence` 6.50→5.55 s, `world_lighting_trace` 22.14→21.19,
  `world_lighting_settlement` 32.94→28.59, `reconstruction` 8.28→8.15,
  `asset.gpu_grid_particles` 6.56→5.11, `asset.gpu_misc` 7.03→5.13; validation
  reconstruction 11.55→11.24 and settlement 33.58→29.53. Leaf counts identical,
  0 `.tmp`, no rewrites; cold logs contain no cache-related errors. Frame_fence
  warm served 38/38 entry points with 0 ms recorded compile in an earlier run.
- Residual: warm `compile=0 ms`, create ≈1.5 ms, pipeline ≈6.8 ms; module
  load/link/specialization, test GPU work, Debug + ASan/UBSan + core validation
  remain. Windows/D3D12 rename/exists branch unrun; no eviction/telemetry.
  Directed windowed runs still hit pre-existing libdbus LSan noise (CTest
  passes). This is a measured backend-codegen saving, not a suite fix.
