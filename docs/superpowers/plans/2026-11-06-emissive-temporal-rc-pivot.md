# Emissive Temporal RC Pivot — Today’s Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans in the main session
> after user validation/resume. Track these checkboxes. Keep one implementation
> context; no fresh author/reviewer pair per small task. One final review.

**Goal:** Finish the playable RC pivot today (2026-11-06, Europe/Paris): world
emissives plus optional last-cascade sky, persistent temporally amortized RC with
previous-field feedback, existing screen-space micro correction and presentation.
Start at4096² stress. Direct tracing remains a bounded diagnostic only.

**Architecture:** Reuse WorldRadiancePass, canonical terrain/Matter sources, native
traversal, existing directional consumers and accepted-submit/fence ownership.
Keep cascade state private and persistent; publish fenced refined-field slots.
No explicit face bake, analytic lamps, direct-tracer fallback or new RHI.

**Tech Stack:** existing C++/Slang/Vulkan stack; RTX2080; Linux Release performance,
Dev correctness separately. **Spec:**
`docs/superpowers/specs/2026-11-06-emissive-temporal-rc-pivot-design.md`.

Implementation is active under the user's 2026-11-07 approval of the spacing-4
adaptive scheduling revision. The original same-day estimates below document
the November 6 review. The current checkboxes and measured acceptance evidence
govern the remaining work.

## Brainstorming feasibility review

**Chosen:** adapt the retained cascade kernels and V22 temporal feedback into the
existing pass, exposing quality controls and measuring4096² immediately. This is
the shortest path consistent with the user's selected architecture.
**Rejected:** wholesale engine/prototype rewrite (too much integration); further
direct/face-bake optimization (wrong goal); many isolated suites/captures (too slow).

Grounded reusable code: world_radiance.slang::worldSolveCascade/worldRefine;
WorldRadiancePass prepare/encode/notifySubmitted/advance/acquire; canonical Matter
projection; signed112B CH consumers; Grid screen-contact pilot; temporal2x/HDR/UI.
Missing: persistent phased RC, previous-field hit feedback, compatible publication
and edit cadence. Thus this is NOT merely selecting cascade_diagnostic.

Two real risks: full-resolution work may not fit10ms, and immutable generation
ownership may require more changes than fit today. Neither is solved by a large
list of tests. Check them in the first45 minutes and on the first GPU capture.
If blocked, state the exact evidence and remaining critical path immediately.
Do not silently switch algorithm, omit feedback/Matter, or call a partial result done.

The side assistant originally self-reviewed under its no-subagent restriction.
A fresh Sol brainstorming builder check in the main session has now reviewed both
artifacts. It found one product question: whether1s requires the affected visible
region to be updated, or only the first visible change. The proposed full-region
response definition remains pending user confirmation; multibounce settling is
separate. This is a design review, not source/performance validation. The checks
covered architecture fidelity, actual reuse seams, work/memory scaling, temporal
hazards, meaningful minimum tests, daylight time budget and all user clarifications.

## Global constraints and review focus

-4096² first = STRESS<=10ms lighting; normal1024²/2048²<=6ms. Preserve4GiB peak
lighting,8GiB device,4K-from1080p/MSAA4 and60Hz goals; no kernel-only budget claim.
-Keep1s visible/edit response target; measure multibounce settling separately.
-World emissives plus OPTIONAL last-cascade sky/time-of-day. No analytic lamps.
-RC field/spatial resolution, spacing, angular samples, levels and ray intervals
are quality levers. Retain screen-space traces to fix visible micro misses.
-Use extra ray budgeting, occluded-probe skipping, fast marching, group-ID/L2
swizzling or wave intrinsics ONLY for a measured bottleneck. No optimization tour.
-Old15%/1% errors are diagnostic, not architecture vetoes. User reviews images.
-Finite energy, bounds, accepted-submit phase changes, history lifetime, and
clean shutdown stay hard checks. Emitter-off and topology changes must recover.
-No broad reset, commits, CPM edits or GPU-power changes. Preserve existing work.
-Review focus: phase/history mismatch; stale light after edits; unbounded feedback;
hidden direct/bake dispatch; screen correction double counting. Covered together below.

## Task1 — Decide the smallest viable4096² configuration (<=45 minutes)

Files: WorldRadiance.cppm/.cpp, world_radiance.slang/types, Editor.cpp;
inspect Game.Config.* and Grid consumers before edits. Use CodeGraph first.

- [x] Freeze current source identities; mark superseded face/direct work dormant.
- [x] Map retained cascade dispatch/binding/publication flow end-to-end; choose
pass-private persistent cascades and reusable refined read/write slots. Never
write a leased/in-flight field. Reuse existing fence bookkeeping; no new scheduler.
- [x] Calculate actual bytes/work for ONE starting quality profile covering all4096².
Start with existing signed112B CH at a coarser exposed probe resolution if it fits;
do NOT require a new32B CH codec today. V22's8B RGB-plus-scalar-transmission sample cannot preserve current RGB
transmission unchanged. Budget existing storage or a compact RGB L/T format;
do not silently discard coloured absorption. Verify layout/round-trip only if
changed; do not make packing a separate prerequisite project. Native
world opacity/emission is not averaged away when reducing probe resolution.
- [x] Record level/interval coverage, update cadence, record budget and retained-slot
peak before allocating. V22's1024²/six-level defaults are not4096² proof. Keep full
world coverage; do not substitute a camera window. If the ownership/capacity path
is not concrete by45min, report the blocker instead of beginning speculative work.

Output: one implementable configuration and ownership decision, not another broad spec.

## Task2 — Implement RC + temporal feedback as one vertical slice (<=120 minutes)

Files: WorldRadiance.cppm/.cpp; world_radiance.slang/types; Editor.cpp;
Game.Config.cppm/.cpp and Game.Application.cpp. Register new files only if needed.

- [x] Add temporal_cascades mode and --world-lighting=temporal-cascades. Keep existing
pass lifecycle APIs, extending settings with the selected quality/update parameters
and explicit injected elapsed/delta time. Do not prescribe a second pass framework.
- [ ] Reuse coarsest-to-finer interval merges and active V22 SuperSampling.update
refinement. Implement staggered probe updates with each stored sample's actual
angular phase; global current-frame phase must not reinterpret old samples.
- [x] Feed previous completed world-field irradiance into hit shading alongside
material emission. EMA stabilizes this field; it does not replace feedback. No
face discovery/bake, explicit ray-tree bounce, or full256-ray production field.
- [x] Permit optional sky only at the coarsest terminal world boundary; rotate/sample
its time-of-day state explicitly. Finite nonterminal misses never add sky, and
lower cascades must not duplicate it. Sky disabled + no emissives produces black.
- [ ] Commit phases on accepted submits; use defined black initialization and
bounded persistent slots. Retain old compatible GI during updates; world/quality
changes invalidate incompatible history. Do not reset the entire field on every
Matter tick. Preserve source identities and coalesce updates without starvation.
- [ ] Build affected closure once and run the first4096² Release capture immediately.
Report actual RC dispatches, per-stage/total lighting time, response and image.
If over budget, adjust one explicit quality/time-budget choice before writing
specialized optimizations. No reference-tracer fallback to make the capture pass.

Output: the intended architecture running at4096², with measured limits.

## Task3 — Connect existing game behavior and micro correction (<=60 minutes)

Files: Editor/Game routing, GridPass bindings, grid_world_radiance.slang,
grid_tiles.slang, grid_screen_contact_types.slang; existing Matter/source/edit owners.

- [x] Feed canonical terrain + committed Matter into the same RC field. Exercise
existing door/emissive-cell changes; don't create analytic lamp entities/tools.
- [ ] Connect foreground/backwall and the existing screen-contact/micro-ray path.
Bound screen work; test thin geometry/disocclusion and RC-only fallback outside
screen evidence. Define composition so correction does not count emission twice.
  The temporal backwall now uses current terrain across the full viewport.
  The GPU scene checks occupied pixels, opening the door, signed correction,
  and RC identity outside screen evidence. Moving solid masks and foreground
  integration remain open. The progress ledger includes the three-size
  Release matrix and startup/deadline misses.
- [ ] Preserve existing temporal2x/HDR/bloom/UI. Camera motion must not clear
world GI; changed topology must not leave stale lighting indefinitely.
- [x] Reuse existing gameplay request/result boundary to consume identified RC
samples where required. Do not invent a mechanic or leave CPU direct tracing
masquerading as the production GI query. If the current seam cannot support this
without a redesign, report that concrete missing obligation before final delivery.

Output: integrated playable RC, not just a standalone shader demonstration.

## Task4 — Consolidated validation and only necessary optimization (<=75 minutes)

Use THREE compact validation units rather than the earlier many-suite plan:

1. One CPU/layout/state case: selected profile bounds/admission, phase coverage,
   accepted-submit versus dropped-submit state; add packing round-trip only if
   storage changed. Reuse existing assertions/tests for unchanged lifetimes.
2. One shared GPU scene sequence: sky off/on, emissive on/off/move, coloured bounce,
   door/thin wall, high-albedo stability and a Matter update. Check finite output,
   feedback contribution/decay, offscreen source influence and correct history.
   This is one setup with subchecks, not seven new fixture initializations.
3. One4096² playable sequence: camera/edit/Matter/micro correction and time of day,
   with snapshots/timing/memory/clean shutdown and dispatch proof that primary
   direct256/face bake are absent. View the output, not just logs.

- [ ] Implement only these missing checks in existing test owners. For changed
behavior observe the targeted failure then pass; no tests mirroring implementation.
Do not build a new test framework, CPU RC clone or mandatory300-frame plotting tool.
- [ ] Run the shared GPU case ordinary and GPUAV once on the coherent final closure.
Reuse valid previous evidence; rerun only affected failures. Keep registered
sanitizer policy. A timeout is an unmet check, not permission to increase it blindly.
- [ ] Use Release validation-off for performance, then separate validation. Freeze
binary/shader/settings identity. No hardware-counter capture unless ordinary
per-stage timing leaves the bottleneck unclear. No duplicate Nsight captures.
- [ ] If needed, optimize ONE measured hotspot: budget/time slicing or quality first;
then proven skip/marching/locality/wave changes only with evidence. Retain only a
material measured gain; abandon an unproductive branch promptly.
- [ ] Capture normal1024²/2048² after4096² when time permits; if unrun, mark normal
qualification open. Record peak live/retired lighting bytes and actual frame cost;
allocation estimates and requested VSync alone are not full acceptance.
- [ ] One final source/architecture review and visual review. Remove only active
obsolete routing; leave dormant diagnostic cleanup for later instead of deleting
large source branches under deadline. Preserve all unrelated dirty work.

## Commands / no testing expansion

Build selected closure with existing Linux build-system setup and VCPKG_ROOT:
`cmake --build out/build/clang-dev-lighting-recovery-20261102 --target engine.tests.app app.game -j 8`
Add asset target only if its changed bindings require it.
`cmake --build out/build/clang-rel --target app.game -j 8`
Use an existing test group or one focused temporal group in App.WorldRadiance.Tests;
register it with the existing helper only if needed. Verify the filter selects the
intended case. Do not automatically run the entire app/asset matrix after every task.
Format/check touched C++ and shader reflection/compilation; required failure-path
checks follow the actual changed ownership, not a generic exhaustive checklist.

Capture flags: --world-lighting=temporal-cascades --map-size=4096
--worldgen-profile=frontier --worldgen-seed=42 --worldgen-report plus existing4K,
60Hz/VSync flags. These new mode flags must exist before execution. GPU work serial.

## Deadline and honest completion

Timeboxes total5h; recompute against actual resume time, do not repeatedly reset
budgets. Prefer less scope in optimization/testing machinery, not omitting RC,
feedback,4096², sky, screen correction or existing scene integration. Stop adding
optional work when the critical path consumes the remaining time.

Today's requested deliverable: working intended renderer, representative4096²
stress evidence, compact correctness checks, captures and explicit limitations.
If it cannot be completed, report what failed and the remaining work by today;
do not claim “done” because midnight arrived or a small fixture passed.

Broader Windows certification, undefined gameplay/media features and unrun original
acceptance items remain in the full-goal ledger. This scheduling proposal does NOT
silently waive them or justify marking the entire historical engine goal complete.
The user validates the concrete delivery/limitations; an unavailable platform is
reported rather than blocking the Linux integration work or being called passed.

Self-review: all user points retained; sky now allowed ONLY in last cascade;
4096² first/stress, quality levers, conditional optimizations, screen micro rays,
minimal tests and deadline addressed. No code was changed by this revision.


## Approval and resume

User approved the revised plan and resumed implementation2026-11-06.
The1s target requires an actual post-edit update across the full affected visible
region and explicitly requested gameplay receivers. Multibounce may settle
progressively. This resolves the review question above. Earlier REVIEW DRAFT/
paused wording is historical; this approval is current. Actual resume21:05CET:
less than3h remain today, not the earlier5h estimate. No missing feature or gate
may be relabelled complete to meet that target.

## Approved adaptive-budget revision — resumed 2026-11-07

User requested implementation resume after the spacing-4 hybrid design and the
six-level clarification. Execute inline with the existing final-review policy;
no renewed execution-method approval, commits or unrelated rollback.
Spec: `docs/superpowers/specs/2026-11-06-spacing4-adaptive-rc-budget-design.md`.
This revision replaces coarse-spacing experiments and fixed quarter scheduling;
all unfinished integration and acceptance tasks above remain required.

### A. Spacing-4 admission and R8UI evaluation

Files: `game/Game.Config.cppm`,
`lib/engine/app/renderer/App.Renderer.WorldRadiance.cppm/.cpp`,
`assets/shaders/world_radiance.slang`,
`lib/engine/tests/app/App.WorldRadiance.Tests.cpp`.
Reuse `planWorldRadianceTemporalProfile`, the source vacuum hierarchy, canonical
Matter overlay and shared GPU fixture. Six configured levels include refinement.

- [x] Extend the existing profile admission case to assert default spacing4,
  six levels and1026² padded4096² refined records; observe failure, change defaults,
  verify pass and record complete allocation demand before4096allocation.
- [x] Query R8_UINT sampled/storage support through existing RHI capability APIs.
  Add bounded skip-table build/read control to existing shader/pass ownership;
  preserve the current marcher as the comparison path. Mirror any new constants
  with size/offset assertions and bind every parameter on both sides.
- [x] In the shared GPU sequence compare identical rays through existing traversal
  and R8UI skip traversal for boundaries, corners, thin geometry, RGB media,
  partial Matter and added/removed obstacles. Preserve hit/entry/progress contracts.
- [x] Measure matched spacing4 Release runs, including skip construction and
  source-version maintenance. Retain only a total-workload gain; report unsupported
  capabilities or an unsuccessful experiment without changing resolution.

### B. Deadline-aware bounded dispatch and publication

Files: WorldRadiance.cppm/.cpp, world_radiance.slang, Editor.cpp,
RenderLane.cppm, Game.Application.cpp and the existing shared test.
Interfaces: preserve prepare/encode/notifySubmitted/advance/acquire; extend settings
with explicit injected time and bounded work allowance. Keep scheduler state private.

- [ ] Add shared-sequence assertions for discarded work, parent-version readiness,
  preserved untouched field rows and oldest-deadline retention across coalesced edits.
- [x] Replace fixed four-frame work groups with bounded per-cascade work items,
  completing required parents before fine consumers. Track source validity apart
  from feedback convergence. Feed accepted source times through the existing lane.
- [ ] Maintain GPU-local work bins/indirect dispatch and asynchronously measured
  cost per work class; include source/table/refinement/publication/screen costs.
  Carry dirty dependency support and mandatory backlog; report overload honestly.
- [ ] Verify whole affected visible/gameplay coverage consumed within1s on actual
  edits, including global-change stress. A source tag alone never passes this gate.
  The generated maps have an actual disjoint-window check at all three sizes.
  It drains every committed Matter tick to a matching camera certificate and
  a signed gameplay read. The 1K/2K/4K measured maxima for coalesced driver
  updates were 797/827/769 ms after the enclosed-terrain change. All 380 gameplay
  requests, including the initial request on each map, completed within 951 ms in
  the final matrix. An earlier 2K run of the shader variant took 1033 ms at startup.
  With the rotating sky and connected intervals, every demanded field on the
  generated dawn/sunset/daylight runs passed GPU coverage. All 18 live global
  hour changes reached matching camera presentation and distant gameplay
  sampling within 335 ms; initial sky requests took 872–951 ms. In the 64²
  one-cell enclosure, the unconnected solver leaked at interior probes. The
  connected solver passed the zero-interior test in the same GPU fixture.

### C. Variability, refresh and conservative inactivity

Files: same pass/shader owners; no new generic scheduler or denoiser framework.

- [x] Add stable-dark wakeup, chromatic change and inactive-region reactivation
  subchecks to the shared fixture; observe targeted failures before implementing.
- [ ] Track temporal coefficient change and sample age; use improvement/cost only
  after deadline work and bounded background refresh. Preserve actual angular phase
  and elapsed-time-aware history. Keep uncertain/influential intervals active.
- [ ] Measure queue overhead and refresh age; tune quota/headroom from evidence,
  without claiming hard GPU preemption or using a guessed fixed ray count.

### D. Complete integration and qualification

- [ ] Complete outstanding terrain/emitter/Matter, sky/time-of-day, gameplay and
  micro-correction obligations from Tasks2–3 above at spacing4/six levels.
- [ ] Run shared GPU ordinary and GPUAV on coherent final source; Release4096
  stress then1024/2048 normal, full lighting budgets, memory, response and images.
  Separate driver memory-report runs on the RTX 2080 paired every reported
  allocation/import with its release through shutdown. Whole-process device-local
  peaks were 1.10/1.37/2.45 GiB at 1024²/2048²/4096², respectively, including
  generation overlap during normal Matter updates. The 4K run peaked at 2.54 GiB
  even counting all Vulkan heaps. See the progress ledger for byte counts and traces.
- [ ] Run touched-source format/style gates and one fresh final review; fix meaningful
  findings and leave unmet/unavailable gates explicit. Do not mark the goal complete
  until the full accepted scope is verified.

Self-review: preserves all revised design requirements, existing immutable owners,
source coherence and deadline semantics. Review focus remains stale skip certificates,
coalescing starvation, stale parent dependencies, sparse-slot corruption and falsely
inactive probes; each is covered by A–C shared-sequence assertions.
