# Fresh-session handoff: cleanup and Oni lighting port

The complete design is owner-approved. This handoff and its implementation plan
were prepared in a documentation-only session. Production execution has not begun.
The owner selected **subagent-driven execution**. Explicit plan-review confirmation
remains pending; preserve that selection when the fresh session starts.

## Copy into the fresh execution session

```text
Implement the owner-approved cleanup and Oni lighting design in this repository,
using the linked plan after confirming my plan review. I have selected
subagent-driven execution; do not ask me to choose the method again.

Required reading, in order:
1. AGENTS.md and codemap.md, then the codemaps for the area being changed.
2. docs/superpowers/specs/2026-11-10-oni-port-design.md — read the entire document.
3. docs/superpowers/plans/2026-11-10-oni-port.md — exact tasks and interfaces.
4. docs/superpowers/specs/2026-11-10-oni-port-complete-builder-review.md.
5. docs/superpowers/plans/2026-11-10-oni-port-execution.md, if it exists, to resume
   from recorded evidence rather than restart completed work.

The fresh complete-design builder check returned “nothing important left.” The
owner subsequently approved the entire design, including its coordinator technical
specifications. Do not reopen approved choices merely because implementation
verification is pending. The approved design wins over historical plans and the
prototype's unfinished experiments. Surface an actual consequential conflict.

Load subagent-driven-development. Use a fresh implementer per task and an
independent task reviewer for spec compliance and code quality, followed by a
whole-branch review. If plan approval has not been confirmed by my launch
instruction, confirm it before production changes. Delegation stays within the
plan; builders do not acquire redesign authority. The explicit cleanup/visual
acceptance stops and BLOCKED_PLAN rules override generic continuous-execution
or unilateral-ruling guidance in the skill.

Apply the plan's YAGNI implementation boundary in every task. Add only facilities
needed by a named approved requirement and its planned consumer; prefer existing
engine facilities and owner-private helpers. Keep the pass owners, command-tail
callback, numeric-status tickets, Lighting Lab, display exercise and Nsight tooling
within their specified responsibilities. Do not build generic registries, scripting,
transaction/telemetry frameworks, speculative caches or extra runtime modes.
Each existing task reviewer records a short YAGNI verdict in the task ledger;
Task 19 rejects unjustified additions and dead temporary/legacy code. Preserve
all requested behavior, supported platforms, failure/cancellation/lifetime handling
and validation. YAGNI does not authorize redesign or dropping required features.

Start at Task 0: commit all local modifications, then verify recovery and inventory
removals. The owner explicitly authorized this initial all-local-work commit,
including unrelated work and changes to code that will later be deleted. The workspace was
dirty at handoff with both useful renderer/recovery work and unrelated IDE/font/
image/documentation changes. Detect the current host/root/HEAD/diff/stashes anew.
Do not assume the previous Linux path or recorded baseline commit is current.
Preserve the stash named “task-6b paused: atlas dilation (pending cascade fix)”
by its actual object ID. Do not pop/drop it, clean/reset the tree, or replace
untracked work from HEAD. Inventory the complete input as commit preflight, then
make the checkpoint before cleanup or creating an execution worktree. Verify clean
status and restoration of that exact commit, including formerly untracked files.
Preserve the original index/worktree distinction in external recovery diffs.

Execute Tasks 0–3 first: retain originals in checkpoint history; keep any extra
archive under a durable external recovery root, never committed under docs/archive
or another repository path. Commit cleanup deletions normally. Remove old integration
through callers/settings/tests/staging, and restore ordinary playable/resizable
gameplay. Present that cleaned application to me and STOP for my explicit cleanup
acceptance. Do not begin Task 4 or the new lighting integration before that gate.
Follow Task 1's concrete production/test deletion list. Extract its nine surviving
histogram/adaptation/bloom leaves into App.PostProcess.Tests.cpp before deleting
App.WorldLightField.Tests.cpp; remove obsolete suite accessors and CTest filters.
Preserve mixed-file material, publication, lifetime and content contracts.

After cleanup acceptance, follow the plan task-by-task and keep the execution
ledger current with actual files/interfaces, commands, exit status, logs, backend,
review results and acceptance. Do not claim a task passed from source inspection.
Run proportionate tests under validation/build-system, with serialized GPU and
profiling work. Preserve Windows and Linux support and list unavailable gates.

Important constraints to retain throughout:
- Use responsibility-based code names: App.Renderer.WorldLighting.*, namespace
  pP::lighting, App.WorldLighting.* tests, world_lighting_* shaders, and
  WorldLighting.* profiler markers. There must be no Oni/oni reference anywhere
  in active code, including filenames, symbols, comments, runtime strings, tests
  and tooling. Keep source provenance/attribution in documentation and preserve
   exact recovery archives outside Git. Audit retained code as well as new
  files; this naming correction was explicitly requested on2026-11-11.
- 1024-square rendered worlds; unchanged ordinary simulation/particle evolution.
- One full-domain source cache, continuous L1–L5 quarter-probe scheduling, every
  fine probe updated each scene frame, one in-place 512-square signed CH field.
- Eight-byte coarse intervals,32-byte CH,4-byte resolved material16/half-Kelvin,
 8-byte source materials, unchanged hot64/cold24 and32-byte metadata.
- Full-domain R8_UINT jump table with0/1/2/4/8/16/32; required backend execution
  qualification is a gate, not permission for an R32 or alternate-tracer fallback.
  Task 8 fixes two dispatches: 32x32 visibility groups (16x16 threads, one32x32
  tile/group, all three levels) then64x64 jump groups. Use160bytes of separate
  level-0/level-1 shared storage, unconditional group barriers and GPU dependencies;
  do not copy the prototype's racy in-place intermediate shared-array reuse.
  Tests check every occupancy bit and the exact largest aligned empty jump.
- Isotropic diffuse feedback; textured normal-mapped visible PBR remains.
  Pure metals have no diffuse feedback but retain metallic response and thermal
  emission. Shared CIE/Planck LUT calibration is6500K → luminance1. Thermal scope
  stays already-opaque surfaces; receive-only transparent cells and gas do not emit.
- Sky hour-1 means black boundary sky, never automatic time.
- Preserve the current probe override and commented odd-extent checks in recovery;
  replace them deliberately with approved inspection and R/S/B handling.
- Independent reconstruction/PostProcessPass, always-on Hable, P1..8 default3,
  R=ceil(W/P), S=ceil(R/2), B=2*S, valid-R post/history, fixed-P crop, native UI.
- Stage A/B/C commit independently on actual accepted submissions. Read renderer
  settlement/fence after each call. Later failure cannot roll back earlier GPU
  work. Ambiguous submit quarantines; small async numeric status must reach the
  application and be inspected during shutdown too.
- No compatibility/migrations, legacy AA/lighting mode, dirty/repair policy,
  per-frame particle/image readback or steady-state wait-for-idle.
- Retired6ms/register/ray/larger-map/no-regression thresholds stay retired.
  Measure the complete application after integration; do not optimize to old gates.
- Task 19 requires full clean clang-dev and clang-rel builds with zero warnings
  or errors; retain and inspect complete configure/build stdout and stderr.
  Inspect complete normal-game/Lab, display-exercise and profiler execution logs
  through shutdown: no warnings/errors and no message flooding at any enabled
  severity. Record message-template counts/rates and representative lines; fix
  emitting behavior rather than filtering output or disabling diagnostics.
  Deliberate fault-test logs are separately labeled. Unresolved findings block
  completion, and unavailable checks remain explicitly unmet.
- Permanent --exercise-display uses production actions and real acknowledgments.
  Permanent scripts/profile_lighting.py supplies bounded Systems/Graphics capture,
  real export fixtures, offline analysis and honest comparison. Counter permission
  is enabled; help output alone does not prove acquisition/export.
- Lighting Lab uses ordinary world loading, solver, tools and checkpoint paths.
  Its exact recipe is in the approved design. I alone accept visuals at the final
  gate; screenshots/tests/reviewers cannot self-approve them.

If a required GPU feature is unsupported, an unexplained compiler ICE occurs,
or the plan needs a consequential architecture/behavior change, stop affected
production edits and return BLOCKED_PLAN with precise evidence/current diff and
the decision needed. Do not accumulate speculative workarounds. Keep independent
approved work possible only within the selected workflow and existing gates.

Task 0's initial checkpoint includes all unrelated local work by explicit owner
request. After that checkpoint, commit only reviewed task changes at the plan's
boundaries; do not sweep new unrelated changes into them. Never commit copied
legacy archives and do not push automatically.
End with evidence, remaining limitations and my recorded visual acceptance,
not a generic “done” assertion.
```

## Artifact links and evidence boundaries

- [Implementation plan](2026-11-10-oni-port.md)
- [Final plan-only comprehension benchmark](2026-11-11-oni-port-plan-benchmark.md)
- [Fresh DeepSeek Pro comprehension benchmark](2026-11-11-oni-port-plan-pro-benchmark.md)
- [Approved complete design](../specs/2026-11-10-oni-port-design.md)
- [Completed builder review](../specs/2026-11-10-oni-port-complete-builder-review.md)
- [Source survey](../specs/2026-11-10-oni-transport-source-survey.md)
- [Three-stage frame timeline](../specs/2026-11-10-oni-frame-timeline.puml)

The earlier comprehension benchmark and initial builder review are historical
supporting artifacts, not competing requirements. The completed review records
the exact reviewed design hash; later status, naming, checkpoint/removal-inventory
and jump-construction edits do not constitute a second independent review.
The owner-requested YAGNI implementation/review gate was added after both
comprehension benchmarks; their recorded plan hashes remain historical evidence.

Planning verified document structure/references and selected geometry/numeric
calculations, and discovered installed profiler commands/SDK capabilities. It
did not build or run the port, qualify a GPU backend, acquire a profiler report,
or obtain cleanup/final visual acceptance. Temporary numeric/diagram previews
under `/tmp/opencode` are not recovery storage or required fresh-session inputs;
reproducible contracts and source-data checksum are in the approved design.

## Before starting

- [ ] Owner has reviewed the implementation plan.
- [x] Owner selected subagent-driven execution.
- [ ] Task 0 committed all initial local work, verified clean status and recovery,
      and preserved the paused stash by object ID; any extra archive is outside Git.
- [ ] Tasks 1–3 execute before any post-cleanup lighting task.
- [ ] Cleanup acceptance is explicitly recorded before Task 4.

Only execution-method selection is complete here; the unchecked items are
future-session checkpoints.
