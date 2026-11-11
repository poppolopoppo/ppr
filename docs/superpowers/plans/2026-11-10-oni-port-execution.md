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
| 1 | pending |
| 2 | pending |
| 3 | pending — mandatory owner cleanup-acceptance stop |
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
