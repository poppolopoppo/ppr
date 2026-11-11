# Radiance recovery implementation plan

> **2026-11-10 status: integrated acceptance FAILED; implementation plan under reassessment.**
> The user's sky leakage, noise, slow convergence, incomplete edit propagation,
> chunk artifacts and Nsight findings supersede any earlier readiness implication.
> Checked tasks and passing component tests below are historical evidence, not
> acceptance of the lighting pipeline. The empty-block optimization proposal is
> paused. No further implementation is authorized by this historical checklist.

## Current reassessment — 2026-11-10

Target: **2D radiance cascades → exposure → bloom → AgX**, with only the requested
Hable temporal MSAA. Preserve particle simulation, the user's sky-hour `-1`
default and all-cell probe visualization, unrelated working changes and the
paused Task6b stash. Current RetroPass odd-extent checks have also been commented
out in the working tree; preserve that user change during investigation rather
than treating the historical rejection as the current complete behavior.

Investigation checklist:
- [x] Extract world-light counters from the supplied export, separating nested ranges.
- [x] Inspect AA mode validation, field scheduling/invalidation, storage and atlas gathering.
- [x] Compare the prototype's active cascade storage/schedule and Oni's active L0 path.
- [ ] Establish isolated GPU attribution and actual resource residency from a capture.
- [ ] Reproduce sky leakage and chunk-edit artifacts through the integrated path.
- [ ] Resolve the fixed-ray-budget/reference-cadence conflict before replacing the design.
- [ ] Implement, capture-verify, review and commit the accepted correction; push nothing.

### Confirmed evidence

1. **Nested GPU ranges, not three additive passes.**
   `out/diagnostics/worldlight/analysis.yaml:39143,39552,40027,40391` reports
   RadianceAtlas 7.266%, FieldUpdate 54.872%, FieldCopy 5.353%, FieldTrace 49.519%.
   Copy + Trace equals Update; Atlas + Update is about 62.14% of the frame.
   Copy reaches 88.3798% VRAM throughput. Trace reports 16.2931% VRAM throughput,
   8.5571% instruction throughput, 3.8932 active warps/SM and 10.9699% PCIe
   throughput. Atlas reports 48.2529% register-allocation launch stalls, alongside
   90.1220% active-warp occupancy. These are distinct constraints, not evidence
   that every pass saturates VRAM bandwidth or that register count is known.
   Trace also contains **106 draws / 13 dispatches**; attribution needs checking
   before assigning its aggregate counters exclusively to cascade compute.
   The export's system-memory-miss rule includes PCIe throughput but no resource
   identity or explicit miss count; it does not establish which allocation spilled.

2. **The sparse temporal adaptation changes convergence by orders of magnitude.**
   `App.Renderer.WorldLightTypes.cppm:136-186` gives 25,165,824 samples at 2048²
   with six levels. `WorldLightField::update` charges four rays per stored sample
   (`App.Renderer.WorldLightField.cpp:373-383`). At 262,144 rays/update, a full
   ordinary revisit takes at least 384 updates (96 at 1024²), before dirty work.
   `world_light_trace.slang:190-201` nevertheless uses the elapsed time of one
   field update for each mature L0 EMA. Under regular 60 Hz updates and uniform
   revisits, its calculated mature-sample e-fold time is about 96 seconds at
   2048² (24 seconds at 1024²), not a measured capture result.
   The prototype updates one quarter per frame (`prototype.wgpu:9712-9768`),
   revisiting in four frames. Equivalent quarter coverage costs 25,165,824 rays
   at 2048²: **96 times the current cap** (24 times at 1024²).
   The inspected Editor scene-preparation path calls the field update each time
   it executes (`App.Application.Editor.cpp:2264-2352`); an explicit convergence
   stop has not been established. Admission/drop behavior still needs runtime evidence.

3. **Dirty rectangles describe source edits, not transport dependencies.**
   `App.Renderer.WorldLightField.cpp:487-499,675-715` maps the source rectangle
   directly into each probe grid. It does not invalidate probes outside that
   rectangle whose connectors cross changed geometry. Those samples retain
   history until ordinary service, then can still take the mature EMA path.
   This is a concrete coverage gap consistent with slow cross-chunk repair;
   it is not yet a complete runtime attribution of the user's artifacts.

4. **Storage and locality diverge from the working reference.**
   `App.Renderer.WorldLightField.cpp:285-320,466-478` allocates two complete
   half4 cascade arrays plus u32/sample state and copies a complete array every
   update. At 2048² this is **480 MiB** for field/state and a **192 MiB copy**
   per update, excluding source/atlas/render resources. The prototype's active
   configuration keeps raw cascades in place plus one L0 temporal array
   (`prototype.wgpu:9206-9266,9723-9768`): 224 MiB at the same six-level,
   2048², half4 dimensions, before any additional engine metadata.
   PPR's per-cell DDA reads a material ID, seven overlay words and material
   properties (`world_light_interval.slang:113-158`). Atlas gathering uses eight
   angular evaluations to read four L0 bins through bilinear gathers
   (`world_light_sample.slang:257-280`); shader machine-code inspection is needed
   to determine what redundant work the compiler removes.

5. **Reference equivalence was overstated.**
   PPR's connector composition has the prototype's near + transmittance × far
   form, but that alone does not prove equivalent visibility, source radiance,
   geometry or sample age. Prototype map radiance includes geometry/material
   shading and a bounded progressive term (`prototype.wgpu:9159-9186`); PPR uses
   an incident atlas gathered at cell centers and fed back at opaque DDA hits
   (`world_light_radiance_atlas.slang:60-92`, `world_light_interval.slang:143-150`).
   Oni's enabled supersampling path skips ordinary L0 and updates every fine
   probe with visibility connectors and packed directional coefficients
   (`oni_v22.slang:6796-6839,9579-9596`). It is materially different from PPR's
   sparsely updated four-bin L0. Sky leakage's exact cause remains unproven;
   another connector-only green test cannot close it.

6. **Competing AA modes are coupled to unrelated post effects.**
   `App.Renderer.RetroPass.cpp:276-323` mutually excludes temporal modes and
   makes auto-exposure require temporal-2x, while bloom requires spatial or
   temporal-2x. Cleanup must remove these policy dependencies along with the
   extra selectable AA paths. Commenting out odd-extent guards alone does not
   verify reconstruction support at odd dimensions.

### Correction direction — proposal, not implementation approval

Reconcile transport, compact shared data, update cadence and temporal filtering
as one reference-grounded design. Keep raw cascade data in place with separate
fine-level temporal state, minimize dirty metadata, and reuse compact optical
and radiance inputs across tracing, atlas construction and shading. Establish
correct visibility and edit propagation before selecting traversal optimization.
Collapse post-processing to the requested Hable temporal MSAA and independent
exposure/bloom/AgX stages. Add accurate grid/post GPU ranges and validate the
range attribution with a capture. Integrated acceptance must cover sealed
sky-lit cavities, sky-off emitters, sustained convergence, cross-chunk edits,
odd output dimensions, particle continuity and resource lifetimes, using
repeatable engine captures with matching camera/settings.

The earlier explicit 262,144-ray cap remains in force until the user changes it.
It cannot simultaneously provide the prototype's quarter-field-per-update
coverage at the current dimensions and four-rays/sample cost. This decision
must be explicit; neither silently increasing the cap nor describing sparse
coverage as reference-equivalent is acceptable. The 6 ms performance target
and visual quality requirements remain unmet acceptance gates.

## Historical plan and evidence (superseded as execution authority)

> **For agentic workers:** Use executing-plans for coordinated, sequential implementation. Builds and GPU execution belong to the coordinator.

**Goal:** Demonstrate emissive radiance-cascade illumination of terrain around and below the starting lamp.

**Architecture:** Keep the existing camera-independent cascades and incident atlas. Use prototype.wgpu's per-parent visibility connectors as the transport model, with explicit final-response units. Repair budgeted invalidation without clearing published radiance that cannot yet be replaced, and retain pending work fairly. References inform implementation, not permission to add an ambient miss term.

**Tech Stack:** C++23 modules, Slang, Slang-RHI/Vulkan; preserve D3D12 compatibility.

**Spec:** Existing task-12 brief and user constraints in .superpowers/sdd/2026-11-09-field-transport-and-wave-occupancy/task-12-brief.md, amended by the fresh Astra diagnosis and the user's instruction to fix GI.

## Constraints
- No global ambient; sky only through coarsest escape visibility. Emission added once; atlas incident-only and bounded at 16.
- L0 spacing two native cells; chunk quads; no reprojection or discarded dirty work.
- Mesh/background integration remains deferred; preserve paused Task6b stash and unrelated user files.
- Target field update <=6 ms at 1024/2048; measure, do not claim from sample counts.
- Engine --capture-frame only for visual evidence. Same scene/camera/configuration and recorded field coverage.

## Review focus
- Budget smaller than invalidation; repeated revisions and edits behind the dirty cursor.
- Sky enabled/disabled with incomplete coarse coverage.
- Emitter/receiver separated by real opaque geometry, not vacuum-only fixtures.
- Radiance mean versus angular integral versus final BRDF; no scalar brightness guess.
- GPU completion/resource bindings and shader runtime compilation.

## Tasks
- [x] 1. Add failing production-budget regressions through WorldLightField::update/readback in App.WorldLightField.Tests.cpp: pending invalidation must make bounded progress; published samples outside the repair slice must not be zeroed. Run red before production edits.
- [x] 2. Repair scheduling in App.Renderer.WorldLightField.cpp and trace reset/update entries: persistent bounded pending ranges, fair level service, direction-parallel work, consistent cost accounting, parent visibility available before fine updates. Test sky/edit/repeated-invalidation cases and existing field suite. Scheduler regressions passed; the sole old-suite failure was a corrected oracle fixture, which subsequently passed in isolation. Final combined suite remains a Task5 gate.
- [ ] 3. Test and implement per-parent-corner connectors in world_light_trace.slang/world_light_sample.slang/world_light_interval.slang against prototype.wgpu:9437-9475. Pin final diffuse normalization with uniform-radiance input; preserve source/bounce semantics. Verify real emitter/occluder/receiver response and sky seam/escape handling.
- [ ] 4. Establish production field coverage and deterministic camera/capture conditions. Capture lamp-on/off or blocked/unblocked terrain at fixed coordinates; inspect shaded images and numerical raw transport. Fix only newly evidenced blockers.
- [ ] 5. Run formatting/semantic gates, affected Dev/Release tests and GPU validation, measure 1024/2048 field costs, obtain independent review, and commit verified steps. Report unmet gates explicitly.

## Evidence ledger
- Initial state: HEAD 88a5e83; uncommitted Task12 Change3 and unsupported /pi removal preserved for correction. No current visual acceptance.
- Python 3.14.9, clang-format 23.1.4, CMake 4.4.5 available. No app.game/engine.tests.app/ninja process found at start.
- RED: clang-rel engine.tests.app build succeeded. Correct-CWD regression run failed preserved-tail at line1897 and starvation at1963; isolated starvation rerun confirmed. Logs /tmp/opencode/rc-recovery-red-tests-cwd.log and rc-recovery-red-starvation.log. Test harness aborted during failed-fixture teardown (134). Initial wrong-CWD run failed shader asset lookup and is not regression evidence.
- Ruling: retain Flash edit-only implementation as previously requested; coordinator owns builds and evidence. Existing dirty working tree remains the execution workspace because it contains the in-flight change under diagnosis.
- Ruling: zero sample counts are the persistent invalidation backlog; preserve published radiance and reserve fair round-robin service. This reuses existing GPU state rather than introducing a separate pending queue. New regressions pin both preservation and eventual service.
- Ruling: use prototype final lightingIndirect (8905-8985), not mapDirectLighting feedback, to establish final-response normalization. No brightness constant is accepted without a uniform-input numerical check.
- Baseline: /tmp/opencode/rc-before-shaded.png + .log, engine capture at1024, sky12, budget262144, accumulation8, retention0.9, atlas consumer0,45seconds. Default overview still dark around lamp. No camera input issued. This precedes scheduler repair; runtime staged shaders were still the old version.
- Capture diagnostics authored by Flash ses_e3f7ff41efferkWBw2Z4BojyBb in Editor.cpp only: fenced per-level count/radiance readback, camera metadata, capture-only field timestamp query owned by FieldLightingState and retained until quiescence/removal. Editor TU compiles; runtime verification pending. Logs rc-capture-diagnostics-build.log and rc-capture-timing-build.log. Editor whole-file format/check/semantic passed before final scheduling build.
- Scheduler Flash ses_e3f827ea6ffeezfISIkios5KsR authored count-only reset, weighted fair quotas, half-budget guaranteed round-robin, direction-parallel dirty/reset, current-parent reads with coarse-first barriers. Awaiting final bounded correction: same-frame repeated sample reads current copied/rebuilt history, not stale previous; retain meaningful >1 accumulated-history regression. Build/test not yet run on scheduler candidate.
- Remaining task3 verification: restore partial-transmittance fold coverage through a real absorbing-medium reference; previous seeded-parent test was replaced by current-parent ordering test in scheduler patch. Do not claim that replacement covers partial transmittance.
- Candidate diagnostic /tmp/opencode/rc-scheduler-shaded.log: source revision2, field_frame4911, cycle136, zero unwritten and nonfinite samples on every level, measured fieldcopy/reset/trace0.924864ms at1024. Captured packet delta0.00037147s; field elapsed/refresh-time semantics remain a potential convergence concern. Camera was externally moved, so screenshot is NOT a matched visual comparison.
- Final scheduler overlap correction reads same-sample history from current copied/rebuilt buffer, avoiding resurrection of stale history. Added high-retention overlap regression and restored >1 prior-history fixture.
- clang-rel app.game/engine.tests.app build passed; format/check passed all5touchedCPP files, semantic blocking0. Full field suite59/60 at47.66s, /tmp/opencode/rc-scheduler-tests.log. All3new scheduler regressions pass. Sole failing replacement CPUfold oracle wrongly assumes spatialuniformity with1x1coarsestgrid. Flash instructed to correct fixture to2levels (complete interior support), not loosen tolerance.
- clang-dev engine.app build passed, /tmp/opencode/rc-dev-app-build.log. No Dev runtime gate yet.
- Task3 TEST-FIRST lane Flash ses_e3f827ea6ffeezfISIkios5KsR: add actual per-parent opaque/absorbing-cell connector oracles via existing evaluate seam plus uniform-input final GridPass response test; no transport/normalization production change before parent RED run.
- Task3 RED confirmed after explicit-conversion test compile corrections: rc-task3-red-tests.log fails opaque connector, absorbing connector, and actual GridPass uniform-response assertions; corrected two-level current-parent fold passes. Combined failed-fixture teardown aborts134 after all four results.
- Local source capture rc-local-source-before.png/.log: actual four-cell lamp at(544,734), emission(2.5,1.4765471,0.44472104). L0 mean below at native(545,727)=(0.4667,0.2636,0.1495); rock probe(545,703)=(0.02991,0.01527,0.00965), all counts8. Field populated near lamp despite dark terrain. Field GPU sample2.349024ms; not sustained budget proof.
- DeepSeekPro read-only reference review ses_e3f570bf7ffeGYN0Ps5VUsRFui requested before geometry implementation: verify offset connector endpoints and whether shaded opaque points should use reference cascadeIrradiance(..., false), rather than self-occluding in-place near traces.
- Additional actual GridPass test with an opaque source matching the rendered receiver fails independently (rc-opaque-red-test.log). Existing uniform-response fixture uses vacuum source, so it isolates units but cannot detect surface self-occlusion. Both contracts are now tested before production correction.
- Minimal /TAU diffuse normalization authorized separately; opaque-receiver and connector fixes await reference-review ruling. No brightness tuning accepted.
- /TAU-only change passes vacuum uniform and opaque-receiver tests (rc-normalization-green.log, rc-opaque-after-normalization.log). Correction: the opaque test's original RED was a units failure, NOT evidence that the first cell self-occludes. The DDA skips its origin cell. Flash is strengthening the opaque test to use a production-length c0 and multiple solid source cells, exposing neighboring-cell attenuation separately. Pro review informed; no neartrace production change authorized yet.
- Strengthened receiver test now RED after normalization (rc-opaque-production-red.log): 64² fixture with valid penumbra8 reproduces 1024² default c0 (0.256m at0.5m/native cell), surrounded opaque source cells. Uniform incident field no longer reaches the rendered receiver unchanged. This isolates the final-consumer neartrace policy from normalization. The reference c0 is ~0.512 native cells at1024, not multiple native cells; fragment positions near native-cell edges can still cross into neighboring solid material.
- Pro review confirms final diffuse uses cascadeIrradiance(..., false); only reference secondary-hit specular uses true (PPR has no such secondary-hit consumer). Coordinator authorizes removing unconditional diffuse neartrace, superseding prior neartrace-preservation decision. Keep existing atlas/debug mean convention.
- Pro confirms connector oracle was geometrically wrong: reference rays run child+d*child.nearM to parentCorner+d*child.farM. Flash correcting both oracle fixtures with production-like offsets and explicit distinction from centre-to-centre before connector implementation. The initial connector RED is insufficient to validate correct endpoint geometry.
- Final diffuse fix passes both actual GridPass uniform-input tests. Corrected connector oracles initially selected the wrong corner in their precondition check; selector repaired, offset-vs-centre optical preconditions now pass and both GPU comparisons fail as intended (rc-connectors-offset-red.log).
- Pro confirms exact hoisting: four DDA connectors/sample +16 parent loads, not16 rays. Coordinator authorizes connector implementation replacing separate child interval, same jittered direction on both endpoints, fixed parent lattice/direct loads, and uniform4-ray/sample budget accounting at every level. Flash implementing with derived-budget test updates.
- rc-diffuse-fixed.png/.log has the same camera transform as rc-local-source-before but still fails visual acceptance. Corrected units alone make the already weak scene darker; no scalar boost is authorized.
- Prepared isolated lamp-off control pack /tmp/opencode/rc-lampoff-hdhuj03z/stylized for --worldgen-root. SHA256 comparison confirms its only changed file is materials/asset-material_showcase_amber_lamp.toml, emissive_strength2.5->0. Source content untouched. Run matched control after connector candidate validation.
- Connector candidate focused7tests pass; complete suite61/64 (rc-connectors-full.log). Remaining three emitter/atlas failures diagnosed: shared DDA now skips connector offset-start cell instead of originating child cell, losing adjacent emissive/reflecting cells. Coordinator approved explicit originating-sample skip semantics, preserving failing assertions and geometry/budgets.
- rc-connectors-shaded.png still visually dark despite rock probe below lamp RGB(.0835,.0447,.0315), counts8, field1.144704ms. Capture display policy confirms HDR+temporal2x+autoexposure, manualEV0, paletteoff.
- New decisive display defect: reduceHistogram and RetroPass::targetEv compute average_log2_luminance-log2(.18), but present multiplies by exp2(EV). Dark scenes therefore darken further. rc-connectors-fixed-exposure.png (autoexposurefalse, EV0) visibly improves terrain at unchanged camera. Flash diagnostic lane now adding independent CPU middle-gray exposure invariant test in App.GridPass.Tests.cpp before sign fix.
- rc-diffuse-lampoff control had matching camera but a hover tooltip over the room: image comparison rejected; apparent brightening was UI contamination, not lighting. User asked to keep pointer outside window.
- New middle-gray exposure regression fails independently at dark_ev>0 (rc-exposure-red.log). Coordinator approved reversing CPU+GPU target EV to log2(.18)-average, retaining exp2(EV) application and existing adaptation/clamps. Exposure lane owns RetroPass.cpp/histogram shader plus exposure tests only; transport lane owns earlier field tests.
- Explicit child-sample skip correction alone did not resolve the three transport assertions (rc-skip-focused.log); no thresholds changed. Shader staging SHA256 matches current source. Failed test teardown exceeded120s after all three failures were reported in5.24s; no process remains. Transport lane adding numerical diagnostics and exact offset-start-emitter regression before any further ruling.
- Exposure CPU/GPU regressions and exact offset-start-emitter regression pass (rc-exposure-offset-green.log). Corrected default-exposure capture rc-exposure-corrected.png shows terrain, but this is not final GI acceptance.
- Transport test diagnostics demonstrated mixed corner coverage (wallT~.613,bounceT~.796), so full-single-hit equality and absolute>.5 brightness assumptions were invalid. Replaced with independent lamp-emission and wall-albedo controls, preserving sky/cavity checks and poisoned-previous-field equality. Final controls corrected to change one variable each.
- User reports horizontal probe projection/storage interleaving and insists on prototype fidelity. Treat artifact as BLOCKING. Probe-only capture rc-interleaving-probes.png shows broad horizontal bands; field-disabled capture rc-interleaving-field-disabled.png has ordinary authored geology without those bands. Pro reviewing actual addressing/storage/reference deviations; no speculative fix authorized.
- New actual GridPass rectangular nonuniform seeded-field reconstruction test passes (rc-spatial-render-test.log), narrowing the sampled-layout issue: affine X/Y gradients plus per-direction B survive rectangular128x64 storage/render/readback. Nonzero origin and actual budgeted tracing remain uncovered by that test.
- User requested independent quick UI lane: no tool selected by default, Colony panel hidden until tool selected. Flash lane ses_e3f23bdefffeMFy62mLGGVfLoI owns game UI/toolstate only. GI lanes continue separately.
- Attempted retention0 control rejected by CLI range[.5,1]; rc-interleaving-no-history.log is invocation evidence only, not a rendering run.
- Pro review BLOCKED visual artifact; flat-buffer indices/ABI/world-to-probe formulas found internally consistent. Concrete warning: plan state-byte accounting stale (u8/sample+u32/probe vs actualu32/sample), to fix before acceptance.
- Corrected review's reference scheduling assumption by reading active prototype macros/passes: temporal=1,sparseupdate=1 (9213-14); cascadeFrameCandidate distributes one of each2x2 packed-texel cohort across wholeworld with randomized phase (9714-17). Coarse hierarchy writes raw current samples; onlyL0 separate temporal buffer accumulates exp(-4*delta) (9752-69). Global sub-cell TAA jitter applies equally to child+parent centres; no angular jitter. Current row sweeps/all-levelEMA/per-probejitter deviate. Pro asked for revised reference-faithful design with explicitbudget/performance tradeoff; no temporal redesign yetauthorized.
- Flash adding a failing raw-coarse-overwrite regression (seed mature stalecoarsehistory, noinvalidations, fullbudgetskyoff ->expectedraw0). This pins the reference contract independently of attributing horizontal bands.
- UI firsthandoff retainedMenu/notice visibility overrides contrary user's ONLYtool rule; coordinator rejected thoseexceptions and requested strictnone=>hidden, withindependenttool/menu state consistent. Awaitingcorrectedhandoff/build.
- UI corrected handoff built with app.game, game.tests, engine.tests.app (clang-rel, rc-ui-coarse-build.log); all 19 game/ui leaves pass (rc-ui-tests.log). Engine capture rc-panel-free-probes-before.png confirms startup has no selected tool and no Colony panel. This remains a pre-temporal-change field baseline, not GI acceptance.
- RED confirmed: field_coarsest_steady_state_overwrites_history fails the raw-zero assertion, rc-coarse-history-red.log; fixture teardown abort exit134 recorded separately.
- User explicitly chose to retain the current ray budget. Pro approved distributed 2x2 virtual-packed groups with persistent bit-reversed group cursor and phase advancement per complete sweep, raw coarse parents, fused L0-only temporal writes, global sub-cell jitter on both endpoints, and actual field-update elapsed time. Flat sample storage remains; no addressing defect found.
- User decisions: immediate replacement on the first dirty L0 repair (explicit prototype temporal exception), preserving untraced published lighting; remove obsolete accumulation/history-retention controls from config/UI/API and use fixed exp(-4*dt) L0 filtering. Existing CLI arguments cease to be valid. No silent no-op controls.
- Temporal adaptation acceptance: preserve weighted ray budget and fair dirty/ordinary service, prove complete sparse coverage without frame-phase aliasing, raw coarsest overwrite, mature L0 EMA, dirty immediate repair and retained unpublished backlog. Horizontal bands, paired bloom-free lamp controls, full affected suites and performance gates remain pending.
- Additional UI evidence: clang-dev app.game/game.tests build passes (rc-ui-dev-build.log), 19/19 UI leaves pass (rc-ui-dev-tests.log). Author checked none/gesture/Escape/NewGame consumers and corrected stale notice comment. Independent UI-only review requested. CLion ColonyPanel.cppm reports ambiguous game.config and unresolved dependent types despite both compiler builds passing; ColonyPanel.cpp warnings-inclusive inspection timed out at15s. Neither inspection is claimed clean.
- Temporal test preconditions corrected before running: mature L0 seeds count8 (zero means immediate dirty repair), coverage checks stored samples rather than treating one group visit as four writes. First build failed u64-to-u32 narrowing (rc-temporal-contract-build.log); test types corrected, second build passed. New three contracts all RED at intended assertions (rc-temporal-contract-red.log): spatial dispersion, partially-covered quadrants across phase sweeps, and exp(-4dt) matureL0. Teardown abort134 remains separately recorded.
- Ruling: authorize Flash production batch for accepted distributed scheduling/raw parents/L0 temporal/global Halton8 jitter/config removal; preserve origin-cell skip until its separate regression batch. Global jitter is in native-cell units, applies to both endpoints, and parent bilinear ratio uses jittered child. Existing hash32 may be reused as part of the approved scheduling adaptation if reference hash source is unavailable; four-phase coverage is hash-independent.
- Ruling: field elapsed time is packet.total_elapsed between successful field updates; first update or clock reset uses dt0 (first dirty repair replaces immediately), no invented1/60 fallback. Reject invalid/nonfinite dt. Field initialization clears timestamp validity.
- Reference review of origin skip: prototype radianceTraceMap samples offset start, interior returns surface radiance; it does not skip origin cell. Proposed next test-first batch: emissive and opaque one-cell probe-start fixtures, then remove explicit sampleOrigin/skip if regression confirms. Existing cell-grid/SDF and incident-atlas/reference-surface-feedback differences remain acknowledged; no capture claim from this source review.
- Independent Pro UI-only review PASS: strict none visibility, startup/NewGame reset, retained hidden notices, typed menu/close/deselect actions, gesture guards, enum stability and semantic grouping verified. Advisory-only existing Escape priority and harmless defensive none branch. This verdict excludes in-progress GI/control migration; IDE ambiguity/timeout remain disclosed inspection limitations. UI commit waits for shared-file temporal edits to settle so unrelated hunks are not accidentally staged.
- Temporal implementation landed and FormatFix/Check + clang-rel app.game/game.tests/engine.tests.app build passed. First focusedGPU run0/5 failed (rc-temporal-focused.log): removal of control words shifted CPU levels40 while shader CB array stays48. Restored two explicit ABI padding words, no fake controls; build/reflection fixture service-handle compile error corrected (rc-temporal-abi-build*.log).
- After ABI fix, rawcoarse, matureL0exp4dt, four-phase coverage and dirty-overlap tests pass (rc-temporal-abi-focused.log). Dispersion diagnostic proves128 samples distributed32perquadrant, row0..31,column0..48; old8-distinct-column assertion was arbitrary for accepted linear bit reversal. Ruling replaces it with exactquota/allquadrants/half-grid spans, preserving originalrow-prefix RED property (rc-temporal-contract-diagnostics.log).
- Reflection size312 standalone vs320 afterGPU setup is genuine target-layout variance: defaultDXBC vsSPIRV session, offsetsidentical48/304/308. Pro source diagnosis confirms targettailpaddingunused. Ruling uses reflectedsize>=CPUextent plus exactfieldoffsets, rather than equality; earlier hypothesis of failure contamination was incorrect for this reflection finding.
- Broad temporal suite61/70 excluding known dispersion assertion (rc-temporal-field-suite.log): skyedge, two connector oracles, three lamp/bounce controls, reflectiontail assertion and two lateRHIinitialization failures. Skyedge independently fails (rc-temporal-skyedge-isolated.log); stated fulltrace fixture still uses samplecount instead of4raycost, correctionauthorized withoutthresholdchanges. Othertransportfailures remain under investigation.
- Candidate enginecapture rc-temporal-probes.png/log: fielddt0.01702768, zero unwritten/nonfinite alllevels, singlefieldGPU3.619328ms at1024. Camera differs from panel-free closeup baseline, so NOT matchedbandacceptance or sustainedperformanceproof. No claim of GI fixed.
- Origin-cell actualupdate regressions both RED (rc-origin-red.log), opaque independentlyRED (rc-origin-opaque-red-isolated.log). Authorized removing sampleOrigin/skip and visitingoffsetstart likeprototype; retain emission+albedo*incidentatlas andconnectorgeometry. Pendingverification.
- Ruling: preserve cycle() publicfullsamplepass contract by incrementing only after fourgroup-sweep phases wrap; m_frame stilladvances perupdate forglobaljitter. Pro stablealgorithm review otherwise approves acceptedtemporal/schedulerstructure; visualproof stillpending.
- Post-origin-policy build passes; focused dispersion/reflection/phase+cycle/skyedge pass, both origin fixtures stillfail (rc-origin-fix-focused.log). Parent found concrete implementation bug: radicalInverse_(index,base) used binary bit extraction for base3 aswell; frame2 jitterY was1/9-.5 instead2/3-.5, moving actualstart towrongcell. Authorized %base and /=base correction, retaining independent known-Halton origin fixtures. This invalidates prior reviewer globaljitter-parity approval until corrected and verified.
- Config migration verification: all31 game/game_config leaves pass (rc-temporal-config-tests.log); all19UI leaves remain green aftercontrolremoval (rc-temporal-config-ui-tests.log). Initial game/config filter matched no configtests, so only corrected game/game_config run establishes configcoverage.
- Halton correction now GREEN for all6focused leaves includingbothoriginfixtures/coverage/reflection/skyedge (rc-halton-origin-green.log). FreshregisteredfieldCTest68/73 (rc-halton-field-suite.log), remaining2connectorgeometricoracles and3wall/atlascontrols. No other failures in this run. Connectororacle migration authorized with independent8phaseHalton table, actualframe, bothjitteredendpoints andupdatedbilinearweights; unchangedopticalsolver/tolerances.
- Matched post-Halton/no-skip fixedEV0,bloomOFF lampcontrols captured: rc-halton-lamp-on/off.png/log. Exactcameraorigin/matrices/viewportanddisplaypolicymatch; both25completefieldcycles, fieldframes2436/2472, budget262144. Emitterpresent4cells vsabsent. Small staticfloorROI(632,397)-(730,410) mean8bitRGBdifference(+1.893,+1.579,+1.429),352positivepixels,0negative; adjacentwallROIspositive too. This supports transported contribution withoutbloom, but smalloverview/differentconvergencephase is not finalvisualacceptance. FieldGPU3.565568/3.446368ms are singleobservations, notsustainedtimingproof.
- First2048capture rc-halton-2048-probes.png/log reports7.550144ms fieldcopy/reset/trace, budget262144,5completefieldcycles, zero unwritten/nonfinite samples alllevels. This observation EXCEEDS6ms; performanceacceptance unmet, no optimizationclaim. Needsattributed/sustainedmeasurement aftercorrectness.
- Connectororacle/diagnostic testbuild initiallyfails runtimeinteger-to-doublebrace narrowing, undeclaredfield andshadowedlocal (rc-connector-oracle-build.log); Flashcorrectingcompileerrors only. Prohypothesis forwallcontrols: obsoletefresh/retention0 assumptions retainlegitimateL0EMA. Awaitnumericdiagnostics beforeauthorizingcount0transport-onlyfixture; noassertionsdropped.
- Migratedconnectororacles stillfail (rc-jitter-connector-green.log). Parentfound evaluate() onlyboundfieldconstantsonceininitialize, soGPUevaluationusedframe0 whileCPUviewreportsframe2. Authorized refreshingcurrentconstants on everyevaluate andcorrectingParentVisibility source/level guards; nooraclegeometry/tolerancechange.
- Isolatedwallnumericdiagnostics confirm retainedEMA: bounced.153564 vsblack.152710 withwallatlas.164062 (rc-wall-bounce-diagnostic.log); seededatlas neutral.0228882 vspoison3.52148 vsatlasoff0 (rc-poison-bounce-diagnostic.log). Authorized restoringexplicitfreshtransportprecondition beforefinalexistingupdate: clearcountmetadata only, retainpoisonradiance, full4-ray-weightedbudget, sameframeallcontrolarms. Keepallnumericandpoison-equalityassertions; matureL0historycoveredseparately.
- Linuxprofilingroutepreflight: onlynvidia-smi found; nsys/ncu/renderdoccmd unavailable. PIX/WinDbg requireWindowshost. ExistingfencedVulkantimestamps providecombinedfieldGPUtime; per-phase/occupancy evidence remains unavailablependingboundedmeasurementroute.
