# V22-informed lighting simplification proposal

> **2026-11-06 USER ARCHITECTURAL PIVOT — supersedes conflicting execution history below.**
> Intended production lighting uses world emissives plus optional last-cascade
> sky/time-of-day, transported by temporally amortized radiance
> cascades with previous-field feedback for approximate multibounce. No analytic
> lamps, separate direct-light pass, production256-ray primary solver or explicit face-moment bake.
> Retain screen-space micro traces for visible fine-detail correction. RC quality
> levers are field/angular resolution, spatial spacing, cascade count and ray
> intervals. Begin at4096² STRESS (10ms target); normal cases retain6ms. Extra
> time/ray budgeting, occlusion skipping, faster marching, L2 group-ID swizzling
> and wave intrinsics are evidence-triggered, not mandatory speculative work.
> Historical15%/1% reference errors are diagnostic, not a veto on RC architecture;
> visual/temporal approximation is reviewed explicitly. Preserve hard safety and
> lifetime gates. The user approved the revised plan and resumed implementation
> on 2026-11-07. The active configuration keeps spacing 4 and six configured
> levels at all map sizes. The optional solar hour now drives terminal sky and
> has been exercised with the visible and distant gameplay windows. Broad
> screen-space correction and cold-frame GPU performance are still open.
>
> [Pivot design](../superpowers/specs/2026-11-06-emissive-temporal-rc-pivot-design.md)
> · [Approved scheduling revision](../superpowers/specs/2026-11-06-spacing4-adaptive-rc-budget-design.md)
> · [Implementation and validation plan](../superpowers/plans/2026-11-06-emissive-temporal-rc-pivot.md)


Status: bounded experiment implemented and connected to the actual playable
showcase; visual and performance qualification remain incomplete. The old exact
implementation is preserved for diagnostics. This reassessment uses newly
available `docs/oni_v22.slang`, SHA-256
`461ba57d80842e6619f1421f034c5c23960ca6fd9ec7921e2d4f23c3e7ccfced`.
Earlier planning used the evidence available at that time.

## Active execution order and stone decision

**2026-11-06 optimized integrated path:** current original-backend Release run
exits0 and captures accepted temporal history with committed Matter, but warm
face moments34.219/32.856ms fail6ms; matching Matter age2911.341ms and camera
receipt2979.329ms fail1s. Direct/discovery/Matter maxima1.565/3.996/0.227ms are
separate component costs. No source edit was needed for this witness. The next
proposed bounded change is parallel rays within each face, preserving summation
order; design `docs/superpowers/specs/2026-11-06-parallel-face-moments-design.md`
is pending approval. Full plan remains incomplete. Evidence:
`out/diagnostics/windowed-native-20261106/optimized-integrated/report.md`.

**2026-11-06 optimized zero-overlap witness:** the original descriptor backend
passes a256-cell camera-only demand replacement: zero intersection,16384 newly
traced rows, zero copied rows,32 direct quanta. Readiness704.757ms and matching
presentation754.470ms; separate core-validation703.715/756.102ms. Direct/discovery
frame-joined maximum2.802ms (validation2.167ms), not total lighting/render cost.
Both runtime exits and exact source restoration/rebuild pass. Three fixed diagnostic
probe demands were explicitly withdrawn at A; sources/occluders and actors stay
unchanged. Sustained/coalesced demand, persistent offscreen requests and integrated
Matter/temporal/full-budget qualification remain open. Next work should address
those rather than optimize the already-passing bounded pan.
Evidence: `out/diagnostics/windowed-native-20261106/optimized-nonoverlap/report.md`.

**2026-11-06 optimized descriptor-batching evaluation:** user approved the bounded
private Vulkan overlay and separate performance/validation measurements; NRI/NVRHI
are out of scope. Matched Release (-O3, NDEBUG, no sanitizers) observations pass
the bounded pan receipt both before and after batching: validation-off readiness
771.022/771.711ms and matching presentation815.408/817.829ms. Separate core-validation
runs also pass this bounded check. Nsight records62.5% fewer descriptor-update calls
and38.2% less API time across the capture, but no end-to-end improvement is shown.
Correctness retention remains pending world/asset GPU checks; the first world run
timed out with an unintended global slow-unwinder override. Historical Debug
failures remain evidence of that configuration, not optimized runtime performance.
See `out/diagnostics/windowed-native-20261106/descriptor-batching/report.md`.

**2026-11-06 native validation attribution:** a matched diagnostic retaining
Vulkan core validation but omitting only Slang-RHI debug wrapping leaves median
sparse dispatch recording at0.258240ms, versus0.260845ms with both layers and
0.020240ms with neither. This identifies native-validation-related CPU work as
the dominant measured dispatch cost. RHI wrapping contributes smaller constant
setup and root-release costs. Native-only readiness1136.668ms and matching
presentation1382.270ms still fail1s. Runtime and eight-source exact restoration/
rebuild pass; both validation layers remain in the restored production path.
Next investigate batching individual descriptor writes without changing shader,
resource lifetime or validation semantics. No production optimization is claimed.
Evidence: `out/diagnostics/windowed-native-20261106/sparse-native-validation-control/report.md`.

**2026-11-06 required discovery readiness:** the existing discovery pipeline is
prepared before field execution only when required by the selected route; lazy
fallback remains. Source review/build/format and full native GPU test pass.
Initial discovery now measures4.092ms after late CPU setup falls to6.078ms.
The matched4096² pan still fails: warm discovery6.400ms and readiness2446.642ms,
rawexit34 beforeB. That capture recorded P8/405MHz memory around the pan.
The subsequently approved manual maximum-performance diagnostic completed:
P0/1515–1590MHz graphics and7000MHz memory, warm discovery3.146ms,
readiness1237.005ms and matching presentation1365.999ms. Higher clocks pass
the measured GPU component gate but still fail one-second freshness. The user
then selected **Auto**; preserve that final policy. The CLI mode query did not
reliably reflect the GUI-held setting. Temporary camera source restored and
rebuilt; the full goal remains incomplete. Policy evidence:
`out/diagnostics/windowed-native-20261106/discovery-readiness/maximum-performance-manual/report.md`.

Subsequent CPU attribution isolates677.664ms of Matter abstract encoding across
16 submissions. Within1408 sparse dispatches, command append/dispatch accounts
for377.060ms, root creation73.893ms, constants45.315ms and buffer binding61.786ms.
These are nested CPU wall-time scopes under Debug/sanitizers, not GPU time.
Warm discovery3.8544ms passes its GPU component budget, while readiness1318.500ms
still fails. Seven temporary source files were restored and rebuilt. A diagnostic
comparison with the existing core-validation toggle off completed: median sparse
command recording fell0.260845→0.020240ms and Matter submit50.150→13.958ms;
readiness793.897ms and matching presentation856.051ms are diagnostic only.
Sanitizers remained active; runtime and exact source restoration/rebuild pass.
Combined RHI/native validation overhead is a major measured contributor, but its
individual layers still need attribution. This cannot qualify validation-enabled
acceptance. No production defaults or performance gates change. Control evidence:
`out/diagnostics/windowed-native-20261106/sparse-validation-control/report.md`.
Evidence: `out/diagnostics/windowed-native-20261106/sparse-encoding-observation/report.md`.
Evidence: `out/diagnostics/windowed-native-20261106/discovery-readiness/report.md`.

**2026-11-06 triangle readiness follow-up:** fresh scene encoding now prepares
admitted triangle variants with the actual target signature, including empty
initial stages. Source review, affected builds, draw/cache and frame-lifecycle
tests pass. Compilation moved into startup, but both full-world attempts stopped
before camera A: initial discovery GPU maxima15.200/15.120ms fail the unchanged
6ms gate. The diagnostic follow-up records P8/300–630MHz around discovery after
25.363s CPU setup; timing alignment is approximate, not per-dispatch calibrated.
No warm-pan or successful lifecycle qualification follows. Temporary hooks were
restored and rebuilt. Investigate required discovery shader preparation before
more timing runs; retain all failures and unchanged budgets.
Evidence: `out/diagnostics/windowed-native-20261106/triangle-readiness/report.md`.

**2026-11-06 main-thread pan attribution:** the bounded main/render buffers completed
without overflow, raw exit 0, and exact source restoration/rebuild. Freshness still
fails at 3726.546 ms. Main update maximum is 3.725 ms; observed service totals
47.414 ms. The first B frame starts at 1323.545 ms, then spends 2372.536 ms in
triangle drawing; matching RHI logs attribute 2359.7 ms to lazy vertex/fragment
compilation. This additional stall does not explain the preceding 1.3 seconds.
The initial admission snapshot straddles startup and is excluded from pan totals.
Investigate pass preparation before changing scheduling or transport. Evidence:
`out/diagnostics/windowed-native-20261106/pan-main-observation/report.md`.

**2026-11-06 pan CPU diagnosis:** Nsight captured Vulkan/OSRT but CPU/scheduling
collection failed; its faster938.698ms pan cannot qualify the unprofiled path.
A bounded deferred buffer of existing frame observations completed with85rows,
no missingCPU/overflow and cleanpolicyexit0. Age2596.348ms FAILS; render-frame
CPUexecution totals522.645ms versus2072.701ms between executions. This narrows
investigation to main admission/service and other work between lane frames;
no pacing/quota/shader fix is inferred. Source review/build/format pass, temporary
camera hook restored. Evidence: `out/diagnostics/windowed-native-20261106/pan-cpu-observation/report.md`.

**2026-11-06 expanded-demand diagnostic/lifecycle corrected:** finite offscreen
screen probes now report pixel unavailable instead of failing valid world-space
receiver demands. The4096²/128-cell-pan run saved4K and exited0 under established
sanitizer policy; all3 probes were explicitly offscreen. Source review and
candidate/restored builds pass; the temporary camera hook was restored exactly.
Freshness still FAILS at1318.987610ms; measured producer maximum3.985248ms
passes6ms but excludes display/post. Recurrent~50ms completion-receipt gaps
require CPU/presentation attribution before another transport optimization.
Evidence: `out/diagnostics/windowed-native-20261106/offscreen-probe/report.md`.

**2026-11-06 fixed-emitter latency corrected:** a function-local table of borrowed
chunk views removes repeated resident lookups during Matter contour conversion,
preserving masks, source coverage and publication fences. Matched actual-clock
move-right564.881ms and move-back558.285ms now pass1s (previous1072.924/1067.874ms).
The same five-capture4K/showcase run exited0 under documented sanitizer policy;
all source/binary hashes match and no sanitizer/Vulkan errors were reported.
Full-output contour parity, attached Matter transaction regression and independent
source review pass. Full Game regression passed140.786s; this does not qualify
arbitrary4096-world edits or close the broader lighting plan.
Evidence: `out/diagnostics/windowed-native-20261106/fixed-emitter-relocation/terrain-lookup/report.md`.

**2026-11-06 fixed emitter movement functionally verified:** the existing
showcase amber lamp now moves reversibly between its two approved slots through
the Driver-owned transaction, preserving source strength/on-off and complete
cell values. CPU/source and attached GPU transaction tests pass; all five actual
4K captures (on/off/on/moved/restored) completed with matching revisions1..5 and
clean rawexit0 under documented sanitizer policy. Source review and final whole
Game suite pass. A stale shipped-palette test was corrected to preserve original
IDs1..63 and explicitly validate amber64. Forward1069.432112ms and
reverse1084.606385ms FAIL1s, so edit latency remains open. This is the approved
small-showcase behavior, not arbitrary generated-world mutation qualification.
Evidence: `out/diagnostics/windowed-native-20261106/fixed-emitter-relocation/runtime/report.md`.

**2026-11-06 cadence policy follow-up and emitter scope:** the expanded-demand
sanitizer-policy run took1405.90999ms to matching present (FAIL1s), then exited33
because a fixed capture probe projected outside the final image. No unsuppressed
sanitizer/Vulkan errors; successful lifecycle is not established by that fact.
The earlier986.700147ms observation remains a narrow pass, not robust freshness.
Evidence: `out/diagnostics/windowed-native-20261106/parallel-rays/runtime/cadence512-lifecycle/report.md`.
User approved the fixed-position emitter relocation design. Implementation,
focused CPU/GPU transaction tests and bounded source review pass; the actual
playable forward/reverse capture is running. The full Game suite exposed a stale
63-entry catalog assertion against the existing64-entry palette; its bounded
test correction still requires verification.

**2026-11-06 cadence correction under verification:** direct replacement and
contributor discovery now retire completed batches before admitting successors
with the existing single actual-frame token. The same expanded-demand pan
observed986.700147ms matching present and3.766272ms peak producer time, with
identical direct/discovery work and at most one producer batch per frame.
This narrowly passes the1s gate in one run; scheduling-candidate lifecycle and
broader acceptance remain open. Raw unsuppressed LSan exit1 is retained.
Evidence: `out/diagnostics/windowed-native-20261106/parallel-rays/runtime/cadence512/report.md`.

**2026-11-06 larger-demand pan gap:** a temporary128-cell camera-pan diagnostic
with persistent512 expanded the receiver union to224x128 because fixed explicit
receiver requests remain active. It traced16384fresh/copied12288old receivers;
matching present1470.951688ms FAILS1s despite joined producer peak4.389216ms.
Direct/discovery completion receipts have median33ms gaps; the next cost
investigation is scheduling/fence cadence, not an assumed shader bottleneck.
This was not a zero-overlap witness. The original camera test hook was restored
and rebuilt. Evidence: `out/diagnostics/windowed-native-20261106/parallel-rays/runtime/cold-pan128/report.md`.

**2026-11-06 persistent direct-window512 approved and implemented.** Both initial
admission and camera replacement now select512 for direct-only window lighting;
bounce and the public settings default remain256. The exact settings match the
previous successful512 witness. Format/style, app.game build and source review
passed. The new documented-sanitizer-policy run exited0, reported no sanitizer
or Vulkan validation errors beyond the existing NVIDIA-only suppressions, and
matched all40 direct work records. Warm pan864.514989ms and frame-joined producer
maximum5.112544ms pass the measured1s/6ms component gates. Warm direct maximum
was0.997216ms. Broader integrated cost/quality/age/memory/cadence/platform gates
remain open. Evidence: `out/diagnostics/windowed-native-20261106/parallel-rays/runtime/persistent512-lifecycle/report.md`.

**2026-11-06 default-quota follow-up:** unchanged default256 passes the bounded
lifecycle check under the repository's documented sanitizer policy (exit0,
NVIDIA-only suppressions, no ASan/UBSan/Vulkan validation errors), but warm pan
freshness fails:1272.939524ms with diagnostics and1090.265914ms in the matched
performance environment. The latter's joined producer maximum is2.917184ms;
80quanta preserve the512 witness's aggregate work exactly. Neither run establishes
default256 age acceptance. Raw matched-run exit1/LSan remains recorded separately.
A [short proposal](../superpowers/specs/2026-11-06-direct-window-quota512-design.md)
to persist the previously measured512 direct-window setting was subsequently approved and implemented above.
Evidence: `out/diagnostics/windowed-native-20261106/parallel-rays/runtime/default256-lifecycle/`
and sibling `default256-matched/`.

**2026-11-06 retained optimization: parallel rays, ordered projection.**
After evaluating the user's V22 jump-texture, shared-memory/wave parallelism and
adaptive-budget leads, the user selected parallel rays first. The
[design](../superpowers/specs/2026-11-06-parallel-direct-rays-design.md) and
[implementation plan](../superpowers/plans/2026-11-06-parallel-direct-rays.md)
use64threads per receiver, four independent directions per lane, shared RGB/status
and the original coefficient accumulation order. Walker and quota remain fixed.
The later skip-lookup proposal prefers encoded VulkanR8_UINT (16MiB/world image)
over literalR32UI, subject to device/binding verification; no walker change is
authorized by this experiment. Correctness qualification passed: affected three-target
build, ordinary candidate GPU (41.002s), GPUAV (89.763s, no validation errors),
asset spatial GPU (251.685s), and independent source review. Small-scene control
coefficient difference remains0, brightness L1=0.05785124 and blocked leakage0.
These do not establish full-world quality. Matched512-row profiled and unprofiled
witnesses each preserve all40 direct work records exactly. Unprofiled warm direct
maximum0.671744ms, frame-joined direct/discovery maximum3.764512ms and matching
warm pan726.262388ms pass the measured component6ms/1s gates. Independent final
review supports retaining the implementation. Temporary Editor512selectors were
restored and default256 rebuilt; these512-row results do not establish default
quota freshness or integrated pipeline acceptance. Both runtime exits remain1
with LSan reports; live VRAM, total rendering, lifecycle and remaining features
are still unqualified. See `out/diagnostics/windowed-native-20261106/parallel-rays/runtime/report.md`.
Evidence: `out/diagnostics/windowed-native-20261106/parallel-rays/`.
The [Turing analysis](../../out/diagnostics/windowed-native-20261106/turing-tuning-analysis.md)
incorporates the user-requested NVIDIA occupancy, swizzling and shader guidance;
cache bottlenecks and final compiled resources remain unmeasured.

**2026-11-06 eight-thread experiment: rejected.** User-approved direct-window
workgroups changed64→8 while preserving each receiver's calculations. Ordinary,
GPUAV and asset checks passed; the40 actual-world direct quanta retained exact
work counts. Median device SM activity increased17%→85–96%, but warm producer
maximum11.702336ms and no-copy batches11.09/11.11ms still fail6ms. Matching pan
present617.652ms passes age only. Low device warp occupancy/instruction issue
remain; aggregate DRAM bandwidth is not saturated. The candidate was archived
and original64-thread shader/pass/tests/maps plus default256 Editor restored;
three-target rebuild passed. No workgroup/quota sweep or promotion follows.
Evidence: `out/diagnostics/windowed-native-20261106/eight-thread/runtime/report.md`.
Broader exposed SM coverage alone does not resolve the transport cost blocker.

**2026-11-06 hardware-counter follow-up:** delegated Nsight Systems capture
reproduced a clean 15.24 ms direct batch with 16.52% device SM activity, 1% device
warp occupancy and 1.51%/0.24% DRAM read/write activity. Its eight workgroups
expose at most eight of 46 SMs; counters support limited parallelism, while
active-SM latency/cache stalls remain unresolved. Another slow batch included
1.49 ms of desktop-context residency. Source/default256 binary were restored
and rebuilt. See `out/diagnostics/windowed-native-20261106/nsight-counters/report.md`.
The user selected a bounded eight-thread workgroup experiment preserving each
receiver's calculations; its [short design](../superpowers/specs/2026-11-06-direct-eight-thread-design.md)
was approved for implementation. The [implementation plan](../superpowers/plans/2026-11-06-direct-eight-thread.md) records the bounded change and verification; no performance acceptance is yet claimed.

**2026-11-06 continuation:** the user approved a ≤1-second freshness scope over
visible receivers plus explicit offscreen gameplay demands, with full-world
source/occluder transport across4096². The November5 direct-native design is the
next transport direction; failed cascade candidates remain diagnostic history.
Implementation reconciliation and the bounded quota experiment are recorded in
[the continuation plan](../superpowers/plans/2026-11-06-windowed-native-completion.md).
Existing strip scheduling, native CH overlap copies and complete fenced publication
are retained. Configurable quota support passed ordinary/GPUAV and the unchanged
physical gates. ONE actual512-receiver experiment saved4K and achieved680.096ms
matching pan present, but warm producer maximum15.999968ms FAILS6ms. Exit1 with
14131-byte/23-allocation LSan remains unclean. Both512 Editor selections were
removed; default256 remains and no sweep/promotion occurred. See
`out/diagnostics/windowed-native-20261106/world-direct512/summary.json`.
Full-world completion remains blocked on GPU cost and the previously open
feature/qualification gates; modeled memory is not live VRAM. The6ms/1s/4GiB and quality
gates remain unchanged. Unchanged immutable source/demand results stay fresh;
wall-clock age starts when identity or demanded coverage changes.

**2026-11-06 requested smaller-map checks:** same binary, 512 quota and 4K output,
Frontier seed42. Warm frame-joined direct/discovery maxima were **5.994496ms at
2048²** and **2.490272ms at1024²**, with matching pan presents578.956/469.114ms.
Startup maxima13.52752/13.262848ms are separate and exceed6ms. Both saved4K,
reported zero VUID/Validation Error matches and exited1 on14131B/23-allocation
LSan. Active receiver counts/content differ, so this is one budget observation
per generated map, not a pure scaling benchmark or full pipeline/cadence pass.
The2K result has negligible margin before unmeasured display/post work. Defaults
were restored to256; the4096² target and its16ms warmNO_GO are unchanged.
Evidence: `out/diagnostics/windowed-native-20261106/map-size-comparison.md`.

**Latest user instruction: defer visual matching and finish lighting and
post-processing at world scale.** Visual rejection is retained as historical
evidence, not the current delivery gate. Do not continue appearance, albedo,
lamp-density or texture comparisons. Complete valid in-flight functional capture
proof, then close native consumer validation and advance scalable transport,
diffuse feedback/media, gameplay illumination, temporal reconstruction,
exposure/AgX/bloom and native UI. Preserve the agreed 4096² world, terrain/matter,
4K-from-1080p/4× MSAA, RTX 2080 8 GB, 60 FPS VSync, quality, resource, lighting
cost and normal-play update-age requirements. Narrow diagnostics and camera-window
direct lighting remain component evidence, not completion at world scale.

**Adopted user budget revision (2026-11-03):** total GPU lighting is now ≤6 ms
per normal-play/camera-movement frame and ≤10 ms for explicitly designated
worst-case stress. A supported 4096² world alone is normal scope. Keep the 60 FPS
target, ≤1 s normal update age, ≤4 GiB lighting storage including overlap, and
unchanged correctness/wall-door gates. Requested VSync does not establish cadence.
Finish valid in-flight overlap/batching work unchanged; then prioritize complete
bounded lighting/post-processing features and reuse applicable validation evidence.
Historical 4 ms comparisons below retain their original interpretation; the next
acceptance run uses the newly adopted 6 ms normal-play budget.

Current technical checkpoint: native receiver-ray direct lighting, compact
4096² source IDs/palette/hierarchy, directional signed CH, entered-face incident
moments and one complete opt-in reflected bounce are implemented. Existing
normal relief, rough specular, medium attenuation and native UI remain. GPU
projection of committed matter and its actual-world field/history capture are
implemented and functionally demonstrated; cost, update age and clean teardown
still fail qualification. Scattering, recursive feedback and gameplay illumination
queries remain open. Canonical textures remain; rejected stone work stays out.

The first indirect reconstruction failed the unchanged 1% sealed/open gate at
1.416%. A paired native first-hit control measured sealed 0/open 0.000761989106.
ONE endpoint-connected candidate then passed ordinary and GPUAV: sealed 0,
open 0.0007842295, **2.9187% energy error** against native, zero VUIDs. Discovery
and replay use each child's exact stored origin; refinement uses actual connector
direction; secondary paths terminate at the black-world boundary. Exact
16/1/64 trace accounting and complete-key lookup remain checked. Missing private
lookup fault injection is source-only. GPUAV CTest still failed LSan (2,228 bytes).
No further join tuning is authorized; direct256/independent1024, zero-albedo,
reflector colour, attenuation, offscreen and material compatibility controls stay.

Bounded sparse pipeline reuse and exact fenced coefficient overlap are retained.
The latter copies same-source/cohort/settings overlap and traces only missing
≤256-receiver strips. Warm/cold field byte identity and independent batch-count
controls pass. Older timing observations are historical; they do not establish
current performance. The direct path remains a selectable control.

Default-off HDR spatial bloom is implemented in RetroPass: scene-unit extraction,
quarter-resolution normalized blur and FP32 glow before existing AgX/exposure,
with native UI afterwards. Ordinary focused controls and the registered HDR
GPUAV shard passed (2/2, exit0, zero VUID/no unsuppressed sanitizer report). The
maximum-HDR test was corrected to its existing AgX golden at unchanged .003;
actual 0.941895 matches expected ~0.9422. ABI/readback/type corrections and the two
pre-test session setup failures are preserved in evidence. Game enables bloom
only in single-bounce-window; no default promotion.

The first combined world trial failed host image admission before any field or
capture. Its new guard had used legacy chain-to-4px mipCountFor instead of the
established full-chain maximum. ONLY that bound was corrected; full/truncated
and malformed-chain controls passed in the existing device-free Grid material
leaf, both affected builds passed, and mean maths/shaders were unchanged. Exact
culprit metadata was not logged, so no material ID is blamed.

The corrected **actual Frontier4096 A/B completed and saved a 3840×2160 frame**
from the 1920×1080/MSAA4 path. Each 128² demanded window discovers 1,701,888 calls /
81,216 records; A/B faces 1,685/1,722, off-window 1,098/1,100. B incident moments add
220,416 outward rays; bounce replay+moments totals 1,922,304 rays, with complete
matching combined publication. B copied 12,288 coefficients and traced 4,096 new
receiver rows. **B request→matching present 1,608.964ms and maximum warm joined
producer frame 23.061ms both fail the 1s/6ms normal gates.** Display/bloom cost,
CPU preparation/uploads and physical scanout are not covered by that GPU sum.
Storage accounting peaks at 2,509,597,948bytes including a 1GiB ESTIMATED external
reserve; full live VRAM is unmeasured. All-pages stress remains rejected before
allocation, not executed. Process exit 1 is LSan 14,091bytes/23allocations, after
capture/gameplay completion. Bloom was selected and its module loaded; isolated
completion/timing instrumentation is absent. Evidence:
`direct-world-window/runs/frontier4096-single-bounce-bloom-mip-fix-20261103/run-summary.json`.

**Next accepted increment: true opt-in temporal2x, not another optimization loop.**
Existing legacy temporal averages MSAA into one raster pixel, so reuse Hable's
four distinct reconstructed output pixels as current 4K signal, then reproject
into separate scene-linear 4K history. Bloom/AgX/UI follow resolved history.
Retro owns R8Unorm/MSAA4 reactive coverage and history; final Grid/Triangle
producers write geometric contributor coverage with independent MAX, including
zero-alpha emission. Current support or any nonzero contributing prior reactive
tap rejects history to exact-current, including vacated pixels; no fake object
vectors/global moving-count fallback. Static camera reprojection is supported;
parallax/uncertain coverage is conservative. Actual bound field publication,
window/cohort, cuts/dimensions/sampling identify history; display-only settings
remain outside linear history. Slot selection excludes BOTH retained and history
roles; history/jitter commit only accepted fresh work. Existing renderer
attachments/signatures suffice; legacy paths stay available. Approximate extra
history+mask storage 214MiB/three slots excludes retention/alignment. Two Sol
owners have disjoint Retro/producer scope, one shared asset-fixture owner, and
Luna serializes builds/tests after both freeze. Focused static, motion, reactive,
retention and display-order controls precede one actual-world proof. Hardware,
quality, resource, cadence, update-age and clean-teardown gates remain open.

Temporal2x implementation checkpoint: source review and affected builds pass.
Ordinary temporal-core controls and the HDR GPUAV pair pass. Actual Grid/Triangle
reactive raw controls pass ordinary after a static-decoration provenance fix;
target0 shading stays unchanged. The fixture now accepts a genuinely completed
submission rather than requiring it remain pending (observed116/116), preserving
unconditional old-owner, image and explicit fence-completion checks. Legacy
spatial Retro/grading regressions pass on their recorded compiled snapshot.
The REQUIRED Grid GPUAV entry timed out at its original300s limit without a
functional verdict; no deadline increase. A reviewed test-only extraction removes
six discarded legacy setup draws/readbacks and keeps all32 raw captures and
assertions. Ordinary Grid finished in212.61s versus222.99s in one earlier
observation; this is not a controlled performance claim. GPUAV still timed out.
One bounded instrumented run completed all32 Grid assertions before entering
Triangle shader setup; outer teardown was not reached. Temporary instrumentation
has been removed byte-for-byte. The unchanged Triangle helper (three captures,
16 assertions) is now relocated to the existing shared HDR leaf. Restored Grid
and HDR ordinary/GPUAV acceptance runs retain their original300s limits; no new
test entry, production cache, framework or deadline increase was introduced.
After relocation, restored uninstrumented Grid ordinary passed215.44s and Grid
GPUAV passed259.99s, exit0/zeroVUID, all32 captures and teardown complete. GPUAV
observed a genuinely pending old-reader fence (110 submitted/109 completed),
then passed the explicit completion wait. This closes the Grid timeout blocker;
it is not a controlled speedup or GPU-cost measurement. HDR ordinary then passed
both leaves in64.12s; registered HDR GPUAV passed both in12.40s, exit0/zeroVUID
under the existing sanitizer policy. No unsupported-integration marker occurred;
the admitted HDR test path executes the unchanged Triangle helper, although
there is no separate capture-counter receipt. All four component gates passed
on the same frozen source/binary. The coordinator released ONLY the two Game
settings sites enabling temporal2x for single-bounce mode; one actual4096² A/B
history/publication/capture proof follows its build. Evidence is under
`temporal-2x/runs/relocated-triangle-required-gates-20261103`.
The legacy baked-view leaf completed its new jitter/admission compatibility
checks, then failed at the archived, bit-identical69.452656% aperture error.
Its final lease check was not reached; the whole leaf is not a pass, and this
old algorithm result is not attributed to temporal2x or native transport.
Game temporal2x enablement is released; actual-world qualification is pending.
The value observation proves real
accepted fence/phase/history binding, not per-pixel influence in game pixels;
shared GPU tests provide separate pixel evidence. First-B age remains immediate,
with capture on a later matching eligible-history frame. No new shader telemetry.

The enabled Game build and independent two-site review passed. ONE actual
Frontier4096 A/B run saved3840×2160 output from1920×1080/MSAA4 with temporal2x,
bloom/AgX and native UI selected. First matching B present was frame19370,
fence6785, request→present1447.138356ms (**NO_GO1s**). A later matching capture
frame19389/fence6791 used real prior6790, phase2, distinct write2/read1 history
slots, field fence6783/bounce epoch2 and publication51/terrain1/joint tick51.
History was bound; per-pixel game-history influence remains unmeasured.
Frame-joined B/epoch2 producer work (25 frames, not a whole-run warm maximum)
peaked16.545216ms on moments/frame19348 (**NO_GO6ms**); no producer stages
overlapped in those records. The19ms sum across12 bounce quanta is NOT a
per-frame cost. Full live VRAM, postprocess GPU cost, cadence and scanout remain
unmeasured. Process exit1 was unsuppressed LSan14055bytes/23allocations; no VUID
text was found. Retain without a tuning rerun. Terminal receipt:
`direct-world-window/runs/frontier4096-single-bounce-bloom-game-enable-20261103/terminal-evidence.json`,
SHA256 `fb3a01c4a3bf3e7df63c26a7e73a01a43beb3981a4dc1bdf331cf59e83bae6f1`.

Next bounded feature is committed dynamic-matter source coverage. First census
actual original material IDs, world cells and physical volumes before camera
culling; no transport reinterpretation until opaque-particle coverage and face
ownership are resolved from that evidence. Reuse committed GPU views, existing
queue/fence ownership and minimal projected readback, not display disks/alpha
or a duplicate CPU lighting pipeline. Homogeneous matter projection, scattering,
recursive feedback and gameplay queries remain open, as do the failed budgets.

The capture-only census is now authored and source-reviewed, not yet accepted.
It projects all committed rows before culling/cached-pack reuse, pairs them with
a Game-owned immutable physical-volume ledger through the exact metadata owner,
and maps only through the allocating device after the exact accepted capture
fence completes. Missing catalog IDs reject; known definitions without transport
remain unavailable. The local submit finalizer now runs exactly once before
capture handling, retaining destructor fallback. Affected Game/app/asset builds
and the existing MatterRuntime CPU ledger leaf passed. First ordinary GPU run
failed a fixture assumption: replacing an admitted catalog/metric in place is
deliberately rejected. ONE test call now uses the existing candidate-world
preparation helper, preserving every capture/assertion and production admission.
Only the asset target is rebuilt; CPU and production-build evidence is reused.
After the one-line fixture correction, resident ordinary passed3.08s, registered
resident GPUAV28.86s and frame-fence18.30s, all exit0 under unchanged limits.
ONE actual4096 census then completed before the original external bound: capture
frame16865/sequence1/fence6563, publication56/terrain1/joint tick56/device epoch57.
All116868 committed rows reconcile into116818 cell/material groups: opaque4389,
absorbing45924, declared vacuum66555, unavailable0 and invalid/outside/inactive0.
Input/grouped physical volumes are12935.45833335311/12935.458333353135m³;
17-digit CSV preserves row values, with ordinary summation-order differences.
The same capture produced4K with real prior6562, phase1, write1/read0 history.
GPU/readback/ledger/projected-vector payloads are each2804832bytes; grouped-vector
payload3739776bytes. These are payload accounting, NOT peak memory or live VRAM.
Process exit1 is still unsuppressed LSan14067bytes/23allocations; timing gates
remain NO_GO. Census CSV SHA256:
`9fe1283ac8e6ad33af8e6247fe9f4fe85956bafb2dceb825fa3a42a7492b1011`, under
`committed-matter-census/runs/frontier4096-census-20261103`.
This completes the bounded inventory prerequisite, not matter transport. Actual
opaque rows now require an explicit coarse coverage/face-ownership decision
before the transport overlay is implemented; do not silently fill a cell from
any grain or convert an opaque surface source into volume emission.

Accepted next model: native entered-face fractional coverage for opaque matter,
and physical-volume-weighted Beer coefficients for absorbing matter. At the
authoritative1/9m³ cell volume, opaque cells are4110×fill1,276×.875 and3×.75;
50 absorbing cells have fill1.875 or2. Keep overfill in summed extinction/emission.
Terrain opaque walls take priority. A partial surface applies coverage C and
surface S (or reflected A×incident moment) once at a genuine entered face, then
transmits1−C; it does not turn surface emission into volume j. Mid-cell starts,
interval splits and corners require explicit ownership tests. Discovery must
collect ALL contributing partial-face keys plus the final hard hit; its old
one-key-per-ray assumption is insufficient. The first increment is a tiny GPU
analytic comparison in the shared native traversal/fixture, before full-world
projection integration. Partial transmission is intentional model behavior;
the unchanged1% blocked-side gate still applies to fully covered walls/doors,
with separate analytic partial-coverage checks and unchanged15% physical checks.

The tiny native-overlay component now passes both existing registered gates:
ordinary48.63s, GPUAV182.02s, original300s bounds, exit0/zeroVUID under registered
sanitizer policy. Six-path source closure adds56-byte immutable sparse cell
records and96-byte source header, shared composition/traversal and all-entry
face discovery. Partial product is.03125; all-key/missing/overflow, unity,
corner/split, depth1/2 overfill, terrain priority and zero-albedo direct identity
checks pass. A real GPU moment bake tests the hard-backed partial virtual face;
positive public direct DC cannot be supplied by black terrain/sky. Full-hard
starts/corners retain their established convention. Partial boundary starts own
their virtual face even with a hard back neighbour; strictly interior starts
do not. Discovery stores12 faces/547 occurrences in the reported overlay arm;
capacity uses checked min(universe,structural,terminalCalls+4P), with per-quantum
occurrences and checked64-bit generation totals. The old bounce control remains
sealed0/open.0007842295,2.9187% energy error. A compile-only twelve-site vector
initializer correction was required; all affected builds then passed. These
are component checks, NOT full-world projection or timing qualification. Evidence:
`committed-matter-optics/runs/native-overlay-analytic-validation-20261104`.
Next is immutable GPU projection from actual committed matter into this same
overlay, coherent publication/coalescing and two actual-world optical epochs;
no position readback, duplicate CPU lighting or restart-on-every-tick policy.
WorldRadiancePass owns that next projection phase and its fenced immutable work.
Reuse the terrain GPU bundle directly; deterministic cell/row-order grouping
produces the same overlay formula. Retain an explicitly provisional terrain-only
bootstrap until a real committed view arrives; committed empty is distinct from
missing. Keep oldest unincorporated edit age across coalescing, and report the
field-incorporated Matter epoch separately from newer live-draw epochs. This
integration is now authorized/in progress, not yet built or qualified.

The GPU projector core now passes the same registered ordinary/GPUAV gates:
36.59s/174.35s, original300s limits, exit0/zeroVUID under registered sanitizer
policy. Real Hot/metadata inputs produce the qualified overlay: six rows,
three cells,.03125 transmission, four changed cells across acquired epochs,
immutable snapshot after later mutation/revocation, weighted optical response,
missing/overflow/empty/fault lifetimes, and513-row grouping counts129/257/127
with a run crossing255/256 and an incomplete final group. This uses the same
application/device and dedicated projection pass; both isolated unknown paths
remain terminal. All affected builds passed. The first execution caught three
buffer getters using a moved-from source; they now use the generation's owning
source, and both policies verify the correction. Initial compile mechanics,
retained-static-owner accounting and fixture identity corrections are preserved
in their artifacts. Review retracted its foreign-device finding: the original
guard was already correct. Evidence:
`committed-matter-optics/runs/live-projection-owner-fix-validation-20261104`.
This is core validation only. Editor/Game scheduling, oldest-pending age,
provisional bootstrap and two real incorporated optical epochs remain required;
the coordinator has now released that world hook. The bootstrap identity must
come from actual preparation through a small immutable binding, never a guessed
revision or a retag of old geometry. Keep live-draw epochs distinct from the
completed field's incorporated epochs. Full-world budget/cleanup gates stay open.

World-hook checkpoint (2026-11-04): Editor/Game now schedule the immutable GPU
matter projection, complete admitted receiver geometry before coalescing camera
demand, and report the actually bound field's source/window/fence and Matter
receipt. Provisional bootstrap stays distinct from committed empty. Showcase
particle palettes follow their retained visual subset; full-world original IDs
remain complete, and unavailable transport must match the canonical source.
Independent source review and affected builds pass. The device-free attachment
leaf passes after correcting its missing visual18 fixture; Grid ordinary passes
all32 captures (226.62s leaf), exact Editor GPUAV regression passes (226.32s),
and frame-fence passes26/26. Grid GPUAV timed out at its unchanged300s limit;
no final leaf verdict, reader receipt or teardown proof was reached, so that
gate remains UNMET despite no logged assertion/VUID before timeout. No retry,
deadline increase or validation-policy reduction is adopted. Evidence:
`committed-matter-optics/runs/world-hook-focused-20261104` and the source freezes
under `source/world-integration-20261104`. ONE actual4096² two-publication run is
released as diagnostic integration evidence, not production qualification.
It must distinguish delivered-request age from full edit age, total producer
GPU cost per actual frame from stage totals, and ledger estimates from live
VRAM. Scalable transport, remaining lighting features and all final world-scale
quality/resource/cadence/cleanup gates remain open.

The single actual4096² run completed with raw child exit1 and no PPM. It DID
incorporate changed committed Matter: first B publication/joint51, device52,
changed416cells, source fence6078/field6304/present6305. Its delivered-request
to first matching result was4979.237ms (a separate camera receipt5242.426ms),
not full edit age. A later incorporated publication177 changed1254cells. The
first application failure was the experiment's explicit first-B-replaced guard
before same-source temporal-history/capture proof; generic131 then propagated
through session delivery. It was not an observed Matter/fence/Vulkan failure.
Epoch2 combined readiness1386.806ms and a moments quantum34.052192ms already
FAIL the1s/6ms gates; epoch4 combined readiness7432.948ms and first-present
packet7449.626ms also fail. Offline join of392 timed Matter/direct/discovery/
bounce rows found distinct actual frames for every quantum: largest warm
producer frame34.052192ms (frame24468, moments). Warm direct max19.588ms,
discovery1.99696ms and Matter0.198656ms; TSR/bloom cost is absent. Cold maximum
171.990528ms is separate. This confirms producer-cost NO_GO. The1.422–1.457GB ledger
and1GiB external reserve are estimates, not measured live VRAM. Full edit age,
normal cadence, same-B history and capture remain unverified. Unsuppressed LSan
reports14103bytes/23allocations: teardown is not clean. No VUID token was found.
Evidence: `committed-matter-optics/runs/frontier4096-optical-epochs-20261104`.
Do not rerun unchanged or add an optical hold: review the experiment's fixed-B
assumption under normal publication, then advance bounded scalable transport
using the qualified native path as a control rather than tuning it indefinitely.

Accepted next bounded correction: preserve first-B receipt/failed age permanently
and track a separate replaceable capture witness at the fixed B camera. A fresh,
fully covered, compatible bound field may prove history while newer work is
pending; retained/provisional pictures cannot. Before consuming the one-shot,
the owner lane checks the prior accepted Retro publication against the actually
bound field/window and extent. The captured result must prove its own source,
publication, accepted history, prior fence, distinct slots and pixel fence.
Normal publication/deadlines stay unchanged; no optical hold. This is bound
history proof, not measured game-pixel influence.

The corrected witness source review, all three affected builds and exact Editor
GPUAV regression pass (153.63s leaf). ONE unchanged actual4096² run saved the
3840×2160 PPM: capture frame20637/fence6455, prior6454, phase0, slots1/0,
history_bound=true, optical publication53/joint53/device54, field6441/bounce3,
window minimum(2016,3104)/counts(128,128). First B incorporated458 changed cells;
its failed age remains preserved:5068.929ms combined-ready/first-present packet,
5085.663ms matching-result acknowledgment,5327.406ms camera receipt. The capture
was a fresh covered-pending frame, not a retained/provisional image. Live draw/
census publication186/joint186/device187 is DISTINCT from optical publication53.
The capture proves accepted bound history, not per-pixel history influence.
The captured-window timing rows have51 distinct actual frames; max producer
frame30.830816ms (moments20590), direct max14.966432ms, discovery1.837152ms,
Matter0.527136ms. These fail6ms; full edit age, post-process cost, cadence and
live VRAM remain unqualified. Raw exit1/LSan14103bytes23allocations means unclean
teardown; no VUID match. Earlier GridGPUAV timeout remains UNMET. Evidence:
`committed-matter-optics/runs/frontier4096-replaceable-witness-20261104`.

After that correction, ONE endpoint-connected PRIMARY RC candidate is accepted
in the existing bounded C1–C3 workspace/shared native fixture. Primary still has
the old shared-direction connector/footprint refinement; successful endpoint
geometry was previously applied only to secondary transport. Separate geometry
selection from radiance semantics: preserve primary emission, attenuation and
sky exactly once, without enabling secondary semantics. Keep native256 control
and face moments unchanged. Require unchanged15%/1%, constant/sky/media/partial
overlay controls, exact counters and admission. Failure stops this candidate;
no phase/density sweep. Pass permits the same actual-world window measurement.
Reduced connector work is an estimate, not a6ms claim; moment cost remains open.
For this endpoint-primary candidate only, close at the first physical world
exit as `(L + T*sky, 0)` before zeroing transmission; later graph edges cannot
re-enter and import emitters. Boundary closure is independent of the secondary
radiance selector. Strict exterior starts yield `(sky,0)`; exact-boundary starts
keep native forward ownership. Interior finite endpoints stay open; insufficient
terminal reach still fails. Preserve native256/moments/straight-primary behavior.
Check vacuum edge/exterior sky, analytic emitting/absorbing prefix plus sky,
opaque blocking, exterior re-entry and exact-boundary inward/outward ownership.

Primary endpoint comparison outcome (2026-11-04): source review, all affected
builds and formatting pass. The ordinary registered shared-fixture run FAILS
the unchanged pointwise brightness gate: relativeL1=.3150826446 versus.15.
Reference1024/native256/candidate sums are.236328125/.234375/.2412109375;
near-matching aggregate energy does NOT cancel the spatial error. All twelve
closed samples remain zero; endpoint statuses, boundary analytics, actual RC
sky/medium/partial equilibrium and exact query accounting pass. The retained
secondary control still passes2.9187%. Leaf49.64s/CTest51.13s, exit8 under the
unchanged300s limit; teardown separately reports2240bytes/12allocations. Evidence:
`committed-matter-optics/runs/primary-endpoint-candidate-20261104/ordinary`.
This candidate family is STOPPED: no GPUAV, retry, phase/density sweep or world
promotion. Preserve native256 and the qualified secondary control. A bounded
planning-owner decision from these failed spatial samples is required before
further scalable-transport edits; quality gates remain unchanged.

Accepted next discriminator: ONE exact-zero source-support count using the
actual immutable terrain+Matter emitter bounds (includingC*S andj), receiver
window/current page masks and complete discovered native face keys. Reuse
`pageMayReach_`; compare receiver-local support against CURRENT page masking,
and face support against the existing128 outward bins, retaining all256/128
quadrature weights. Nonzero sky, inside-source or uncertain support stays in.
Count only: no CPU ray tracing, position readback, culling or scheduling change.
Check retained matched artifacts first; if absent, measure once after discovery
of the first complete nonprovisional committed generation, before moments.
Report per-quantum/full-generation included/zero-proved work, exact identities,
CPU cost/bytes and optimistic packed-batch frame count. The screening model
assumes equal cost per retained ray: current maxima imply roughly2.49× direct
and5.14× moment reduction before overhead. Those are planning estimates, not
measured bounds or performance acceptance. Include nondirect/fixed latency in
the optimistic60-frame/1s schedule: unchanged64 direct quanta alone cannot fit.
Insufficient headroom stops this bound-only variant, without bound subdivision
or mask tuning. Only a promising result permits ONE exact-zero elimination and
bounded work-based batching increment, followed by actual6ms/1s and unchanged
native numerical/physical gates. Counts alone qualify none of those targets.
Retained artifacts lack the matched full support/mask/face-key data, so the
single actual-generation count hook is authorized. A small completed-fence face
key readback is permitted only if existing discovery ownership lacks CPU keys;
Hot/position/full-source readback stays excluded. Retire only the rejected
unpromoted five-path primary candidate using its verified archived preimages,
preserving its manifest/source snapshots/failing result. This restores the
qualified required fixture before the independent count change; original
native15%/1% and secondary controls are not removed or weakened.
Count seam approved: reuse discovery's existing optional key copy, map once
after its exact completion fence and retain owned CPU keys in the immutable
result through scratch cancellation. Charge retained vector capacity/readback
bytes including overlap before admission. Count against the same direct plan,
source/window and face-list identity before moments. Append an optional
synchronous diagnostic clock to existing `prepareBounce` (never stored), with
Editor's existing call supplying `observationClock`; null-clock fixture timing
is explicitly unmeasured. A small optional result on existing BounceDemand
supports independent fixture assertions and logging. No culling/scheduling or
new asynchronous protocol is part of this increment.
Model clarification from current run-loop source: application frame IDs also
advance on admission skips, so raw ID spans cannot represent60Hz elapsed time.
Do not add frame-span metadata or a raw-frame latency input. Retain exact WORK
quanta and an optimistic packed required-submission lower bound only. More
than60 rejects the variant; at most60 remains UNQUALIFIED without measured
fixed latency/cadence. First finish the ray-count discriminator. If it is
promising but fixed latency is unavailable, resolve that evidence before any
culling/batching release; no hidden latency-model pass or weaker1s product gate.

Count-hook checkpoint: source review, named formatting and all three affected
builds pass after correcting a routine TimeSpan/TimePoint start-type error and
three authored paragraph separators. Existing ordinary/GPUAV shared leaves
both pass functional assertions (43.52s/180.27s); GPUAV reports zero VUID and
Validation Error matches. BOTH CTest processes still fail teardown with LSan
2240bytes/12allocations, the same allocation pattern observed before this count
change. No suppression or retry is adopted; the cleanup gate remains UNMET.
Evidence: `committed-matter-optics/runs/bound-support-counts-20261104` and
`source/bound-support-counts-20261104`. ONE unchanged actual4096² count diagnostic
is released under the existing180s/240s bounds, not as production qualification.
Runtime emits generic counts/work caps only; historical GPU timings and product
budget screening remain offline. No lighting query or scheduling change exists.

**Count-discriminator result (2026-11-04): REJECT the bound-only variant.** ONE
unchanged actual4096² run completed the count hook and saved a matching3840×2160
PPM (frame20968/fence6406/publication186/joint186/device187, window
minimum(1984,3104)/counts(128,128), faces1685, emitterBounds3, 6,115,428 support
tests). Direct support is 3,936,256 baseline /3,839,213 included /97,043
zero-proved (**2.465%**, 1.025× retained reduction). Moment support is 215,680
/199,472 /16,208 (**7.515%**, 1.081×). The optimistic required-work lower bound
is **92 actual-render frames**, above the60-frame ceiling. The predeclared2.49×
direct and5.14× moment planning reductions are not present, so headroom is
insufficient. No exact-zero elimination, work-based batching, bound subdivision,
mask tuning, culling, scheduling change or weaker1 s gate is authorized. The
direct path remains the control; native15%/1% and secondary controls are
unchanged. Zero VUID/Validation Error matches, but process exit1 retains
unsuppressed LSan14,103bytes/23allocations (external-module/libdbus class), so
clean teardown remains UNMET. Counts are diagnostic only: fixed latency/cadence
UNMEASURED, GPU calibration OFFLINE, and no6 ms/1 s/4K60/VRAM result follows.
Evidence:
`committed-matter-optics/runs/bound-support-counts-frontier4096-20261104/offline-screening.{json,md}`.
The coordinator now moves to native consumer validation closure before any new
scalable-transport architecture is proposed.

**Native consumer validation closure (2026-11-04): Grid GPUAV PASS.** One
unchanged registered run of
`engine.tests.asset.spatial_grid_gpu_validation` on the current rebuilt asset
binary (SHA-256 `4b35b708f84f6884cd5bf10b621452ea260de2193cda19591eb1be64e1ea8b5a`)
passed 1/1 in **265.69 s** under the original 300 s deadline, exit0, zero
VUID/Validation Error matches, with only the registered NVIDIA suppressions
(8+4 entries). No deadline increase, policy reduction or assertion removal was
used, and no new test entry was added. This closes the world-hook Grid GPUAV
timeout (previously 300.40 s, binary `2ad09b60…`) as a functional gate on this
source/binary. It is one run, not a controlled speedup or GPU-cost measurement;
cold shader/pipeline compilation still dominates, app-side LSan teardown remains
unclean on the world_radiance leaves, and no6 ms/1 s/4K60/VRAM result follows.
The next bounded feature increment is renderer-independent gameplay illumination
queries, beginning with a CPU local-source query over the qualified optical
module and analytic controls; no GPU transport or culling change is implied.
Evidence: `committed-matter-optics/runs/native-consumer-grid-gpuav-20261104/summary.json`.

**Gameplay illumination CPU query (2026-11-04): component PASS, not yet
consumed.** `pP::lighting::WorldRadianceGameplayQuery` now captures an immutable
bounded (≤256 native cells/edge) CPU terrain window in global cell order from a
`WorldRadianceSource` and traces normalized rays through the existing
`OpticalHierarchy`/`traceOpticalAccelerated`. Overlay composition follows
`worldCell` for full and zero coverage; a partial-coverage row (`0<c<1`) on a
non-opaque base is rejected `not_supported` rather than silently approximated,
while an opaque base still ignores its overlay row. An all-zero sky makes the
window exterior opaque black; any nonzero sky makes it vacuum and adds
`transmittance*sky` once per channel. Escaped transmittance is the medium
transmittance for a vacuum exterior and zero for an opaque exterior. Matter/
projected particle rows remain GPU-only and are outside this CPU query.
Thirteen analytic controls run in the existing `app/lighting` group (39/39) with
no new test entry or CMake change. Independent review returned PASS after two
fix rounds; the registered `engine.tests.app.functional_cpu` entry passed 1/1 in
70.23 s, and build/format/semantic-style checks passed. The Game does not consume
the query yet: at normal play the source is Editor/render-lane-owned and the Game
drops its handle on `WorldJob` retire, so consumer wiring needs an explicit
request/result across that boundary or a changed source ownership decision. No
GPU, 6 ms/1 s, 4K60, VRAM or clean-teardown result follows. Binary SHA-256
`01e072fd16a2c600158413fca24751f2117a84afdae888d5f67c914b59b6a87a`.

**Aggregate particle-emission admission + receipt (2026-11-04): component PASS,
not whole-scene HDR safety.** `pP::lighting::admitWorldRadianceEmission(cell_count,
per_cell_maximum)` now validates the observed per-channel composed per-cell
emission maxima from the existing matter summary (`summary[22..24]`) and the
observed participating overlay-cell count (`summary[8]`), and records
`WorldRadianceEmissionAggregate` on `WorldRadianceMatterStatistics`
(`m_emission_maximum`, `m_emission_bound`). The bound is `cell_count × per-cell
maximum`, a conservative upper bound on the consumed per-channel aggregate, not a
measured sum. Negative/nonfinite maxima and nonfinite/unrepresentable bounds
reject the quantum before publication; nothing is clamped or omitted, and the
existing nonfinite `summary[0]` status still fails closed. The fields travel to
`WorldMatterLightingReceipt.m_statistics` without a receipt edit. Four analytic
controls (records count/maxima/bound, zero cells, negative/nonfinite, exact
represented bound vs unrepresentable rejection without clamping) run in the
existing `app/lighting` group, now 43/43; broader `app` is 44/44. Build and
format/semantic-style gates passed; no CMake/CTest entry was added. Independent
review PASS with advisory-only findings (bound is loose and not a measured sum;
admission precedes the `summary[0]` log/status so a multi-fault readback reports
the aggregate error first; no GPU integration assertion). Relinked test binary
SHA-256 `215b18d736d8dce511b028abb4682ee23584db76fd1e9d3e3d86c9b158d3c215`.
The broader `--filter app` run failed only because GPU leaves resolved shaders
from the repo-root cwd; it is not a regression of this change. Whole-scene HDR
safety, measured-scene accumulation, GPU integration, pre-exposure, 6 ms/1 s/4K60,
VRAM and clean teardown all remain open.

**Aggregate pre-exposure admission (2026-11-04): component PASS, not whole-scene HDR safety.** `pP::lighting::admitWorldRadianceExposure(const WorldRadianceEmissionAggregate&, double)` now applies a finite strictly-positive display pre-exposure scalar to the observed aggregate emission bound. It independently rejects a nonfinite/negative/above-FLT_MAX recorded bound (`argument_out_of_domain`), a zero/negative/nonfinite multiplier (`argument_out_of_domain`), and an exposed result that is nonfinite, above FLT_MAX, or underflows from a nonzero bound to an FP32 zero/subnormal (`result_out_of_range`); nothing is clamped. At Editor receipt construction the admission runs with live `std::exp2(double(m_retro_settings.m_exposure_ev))` before either `state.m_combined_source` or `state.m_matter_receipt` is assigned, so a rejected bound publishes neither. No rendering/Retro/bloom/AgX behavior changed and the exposed array is an admission gate, not a second radiance value. Four analytic controls (scaling, zero bound stays zero, zero/negative/nonfinite multiplier, and malformed-bound/nonfinite-product/FP32-underflow rejection without clamping) run in the existing `app/lighting` group, now 47/47 (43 prior + 4); broader `app` is 48/48. Format, semantic-style and affected app/test builds passed; no CMake/CTest entry was added. Independent review returned PASS after two fix rounds; the final test-only coverage and clarifying comment were applied after that verdict. Relinked test binary SHA-256 `7033e06bea86d3ca139f817699198fa967523ad1a85be2fd61ac746c37feb4e5`. Whole-scene HDR safety, measured-scene accumulation, GPU integration, 6 ms/1 s/4K60, VRAM and clean teardown all remain open.

**Gameplay illumination query consumer (2026-11-04): cross-lane boundary PASS; diagnostic only, not a gameplay mechanic.** The Editor/Game now carry a value-only `WorldRadianceGameplayQueryRequest` (nonzero sequence, world-XZ centre metres, bounded edge, expected world/revision) across the existing render lane and return a value-owned `shared_ptr<const WorldRadianceGameplayQuery>` receipt. The lane resolves it only from the exact immutable `bound_lighting` source/cohort already stamped into that frame, so the Game never receives, retains, or dereferences a `WorldRadianceSource`; it reads `width()/height()/cellMetres()/minimumMetres()` from the returned CPU query, which may outlive the Renderer. A zero sequence is `invalid_argument`, zero edge `argument_out_of_domain`, absent source/cohort `not_connected`, and a mismatched world/revision `operation_canceled` with no query. `WorldRadianceGameplayQuery::windowForWorldPoint` clamps the centre cell and window to the source extent and the edge to 256/source extent, rejecting nonfinite centre or nonpositive cell size; capture errors propagate unchanged. The Editor consumes the request exactly once at frame-request construction, so only one submitted frame carries it. In the Game the request identity is the `(world_generation, geometry_revision)` pair (revisions reset to 1 on a new world); a matching receipt is accepted as explicit last-good, unmatched/stale receipts are never applied, `operation_canceled` re-arms from the frame actually bound, other errors suppress that pair, a dropped frame keeps the armed pair for retry, and workshop-suspended or foreign-epoch settlements observe or abandon the transport before their early return so the sequence cannot strand. The first consumer traces one normalized ray at the player cell and logs a structured `diagnostic_observation_not_gameplay` record; it changes no simulation. Seven transport/window/capture controls run in the existing `app/lighting` (50/50, app 51/51) and `app/render_lane` (39/39, app 40/40) suites; `app.game`, `engine.tests.app` and `engine.app` build clean, format check and semantic style (Blocking 0) pass, and the test binary SHA-256 is `13e571303d7934f0f548a98f783a16b326fe84129b9cc2bd88aa4b9da1d1c0c8`. Independent review returned FAIL twice (world/revision pair keying, retained-frame retry suppression, stranded inflight on workshop/epoch skips) and PASS after the fixes. No new CTest entry. The Game state machine has no direct unit test (the lane test supplies its own executor); partial-coverage non-opaque rows remain `not_supported`; a real gameplay behaviour consuming incident light is still undefined by the plan; and GPU/world-scale cost, 6 ms/1 s, 4K60, VRAM, diffuse feedback, media and clean teardown remain open.

**V22 spacing-relative cascade interval base (2026-11-04): REJECTED.** ONE registered ordinary leaf exercised the only remaining unexamined v22 numeric divergence with everything else frozen: four-level cascade, native-cell refined spacing, phase 0.5, unchanged merge formula, four-cell footprint box, probe lattice and source masks. Only the coarse-level near/far base changed to v22's spacing-relative ratios — `(1,4),(4,16),(16,64)` instead of `(4,16),(16,64),(64,256)` — retaining the top-level `far=max(far,2*ceil(maximum_l1))` world-escape extension. No shader change, no new CTest entry, no interval/phase sweep. The run admitted both states correctly (transport=3, 3 coarse dispatches, spacing 1, 3896 rows, 37504 attempted queries) and exited non-zero only at the unchanged brightness gate. Open L1 was `0.6908009426652892` against `0.15`, with signed bias `-0.1370372772216797`, open measured `0.09929` versus direct `0.23633`; closed stayed exactly zero (`leakage_pass=true`). The straight256 control reproduced `0.05785123966942149`, confirming an unchanged fixture and oracle. Interval base is not the remaining cause and makes brightness materially worse, so no interval or phase sweep is warranted. The transport enum and test helper were retired after archiving the source delta and the raw CTest log under `out/diagnostics/environment-lighting-20261102/v22-reassessment/v22-interval-base/`; format fix/check and semantic style (Blocking 0) passed, the affected build passed, and the registered ordinary leaf returned to green (exit 0, 39.79 s, 0 validation errors) with relinked test binary SHA-256 `1a2271cd35655a90695c36078b7f6a541634fd14e183664374fdd715fb233631`. The experiment binary that produced the result was `26994cbcc02aa2e65fa9c62fd19bd986a88d95cb32856fa164a1e9479fd740d6`. This does not qualify scalable cascade reconstruction; the endpoint-primary family, count variant, fixed-probe spacing-2/spacing-1, phase and terminal-C1 candidates remain rejected, and 6 ms/1 s/4K60, VRAM, diffuse feedback, media and clean teardown remain open.

**Earlier user steering supersedes the older stone acceptance below:** reject
stone v2 and defer material textures to a future session. No canonical promotion
or new bay texture/dressing work. Focus on shipping lighting inside the real game
world at scale. Complete and measure the accepted bounded sparse pipeline reuse,
then discriminate scalable cascade reconstruction in one tightly bounded
comparison before production-scale integration, temporal reconstruction,
remaining lighting features and full-world budget qualification. All agreed
resource, quality, one-cell wall/door and temporal-age targets remain unchanged.
The v2 evidence is retained as rejected work, not a production dependency.

The bounded sparse program/pipeline reuse is now implemented and measured.
Seven owning pairs are copied from the current generation; all mutable buffers
remain fresh. Source review, affected app/test builds and both focused GPU leaves
passed (ordinary 8.11 s; Core+GPUAV 9.83 s, no reported VUID/errors). Two test-only
corrections preserved explicit expected assertions and the registered validation
default: unset or 1 enables both layers, explicit 0 disables them. No CMake or
fixture framework change was needed. Private partial/foreign donor rejection is
source-reviewed without new fault-injection coverage.

Three canonical baseline runs versus three uncontended candidate runs gave
OFF median 1339.594→1018.949 ms and restored-ON 1342.133→1024.008 ms. Candidate
ranges were 1007.763–1021.924 ms and 1017.541–1024.635 ms respectively; every
transition still misses 1 s. This is application present-result receipt time,
not scanout. Captures were byte-identical to baseline. Each candidate built 7
pairs once and reused 7 with 0 newly created per replacement; nine reuse-call
CPU observations, including initial admission, had median 2.520 ms and
range 2.206–10.584 ms. Those timings include allocations. This does not claim
GPU-frame-time savings, 4 ms, 4K60 or full-world qualification.

Candidate run 01 was excluded from timing because an IDE query overlapped it;
runs 02–04 are the uncontended samples. Existing libdbus LSan exits remain
unsuppressed (one run 1626 bytes, otherwise 1566 bytes). Earlier IDE checks reported
no errors with warnings, but a later Sparse.cpp check timed out; its final
inspection remains unavailable and the IDE used a separate Debug profile.
Evidence is `service-lamp-switch/runs/pipeline-reuse-canonical-comparison-20261103.json`
and the adjacent `clion-inspection-reconciliation-20261103.json`. The next step
is the bounded cascade diagnostic below, not more latency or texture tuning.

The next cascade discrimination is bounded and diagnostic only. Test whether
the far record must retain transverse crossing position as well as direction.
Reuse the existing 24 closed/open points, 256 midpoint directions and independent
1024-ray reference. Split each original ray at its first forward dominant-axis
plane aligned to four native cells. The control continues the original ray;
it must reproduce straight-256 within the existing traversal tolerance. The
candidate starts its same-direction far interval at the native transverse
crossing cell centre on that plane, displacing it at most half a cell, and
composes the unchanged near interval before aggregation. Use ray-forward
ownership and existing world-escape handling. Report unique reusable far keys
and both directions of emitter-hit classification changes.

This is at most 6144 point-direction observations / 18432 traces before reuse.
Invalid controls make the test inconclusive; candidate error above 15% or blocked
leakage above 1% rejects it with no density/phase sweep. A pass permits a separate
two-level shared-table trial, not production acceptance. One full 4096² level
with planes four cells apart, 16 directions and 16-byte intervals is estimated
at 1 GiB before guards; 256 resident directions would be 16 GiB. No production
plane-grid allocation, scheduler or renderer rewrite is authorized by this
diagnostic decision. Any production proposal must satisfy the unchanged overlap,
GPU-work and age budgets. Run this only after the bounded reuse measurement.

**Result: the bounded transverse-plane candidate is REJECTED.** Ordinary and
Core+GPUAV runs completed with the same result and no reported VUID/validation
errors. The original-ray split control matched straight-256 exactly (maximum
error 0), with 5.7851% L1 versus independent 1024 rays. The whole-native-cell
transverse-centre candidate was valid but had 58.6777% L1; both arms had zero
blocked leakage. No tuning or two-level table trial is authorized by this result.
There were 6144 observations, 36864 physical/metadata rows and 18432 helper trace
calls, with 3976 cohort-local unique far keys but no actual table reuse.

The open state gained 8 and lost 15 far-emission classes through transmitting
near intervals. At (9.5,3.5), reference=.056640625, control=.0546875 and candidate=0;
at (9.5,4.5), reference=.01953125, control=.0234375 and candidate=.046875.
Keeping the direction and crossing-cell ID therefore does not preserve which
original rays see an emitter through the aperture. This failure is not an FP64
or irregular-boundary issue and is not a measurement of v22 itself. Preserve
original-ray direct visibility in the next architectural decision; do not infer
that more exact contour machinery repairs this information loss.

Evidence/source are under `plane-transverse-junction/` in the diagnostic root.
Both registered 300 s CTest entries ended with the intended physical assertion
`plane_verdict[2]`, not an infrastructure timeout. Production shader entry bodies
and shared renderer remain unchanged.

**Accepted next architectural direction:** preserve original receiver-ray
first-hit visibility for direct emission/sky; reserve cascade reconstruction for
reflected/scattered radiance, counted separately so direct emission is not doubled.
This is a direct-component implementation tranche, not scalable indirect RC
acceptance. The coordinator accepted Astra's proposal for world-registered 32×32
receiver pages with conservative emitter direction masks from actual immutable
terrain/material data, including offscreen sources. Reuse 256 midpoint directions,
native DDA, vacuum hierarchy, signed CH and generation/fence ownership. With
black sky, culling includes all surface and volume emitter bounds; uncertainty
includes the direction. Nonzero sky initially requires all directions and
explicit measured cost. Media limitations stay explicit.

The next source scope is being established read-only in the real game producer,
not another small-scene reconstruction helper. Publication must cover the complete
requested visible/offscreen page set at a matching revision; door edits initially
invalidate that set conservatively. Keep the separate full 4096²/all-pages stress
qualification and unchanged 4 GiB overlap / 4 ms per frame / 1 s age / 15% / 1% gates.
Measure page demand, actual rays/DDA visits, all retained bytes, per-frame GPU
work, backlog and request-to-matching-publication age. Include camera motion,
offscreen emission, a door edit and a distant narrow source. Direction culling
must reproduce straight-256 on the existing holdout; finite angular sampling
and source density can still defeat the full-world gates.

Current native source already stores packed 2-byte IDs, 16-byte palette rows and
the existing dense u32 vacuum pyramid; do not add a parallel compact source.
The current 64×36 source is 2304 IDs captured from one 128² upload and is not the
proposed 32² page or a full 4096² field. Existing experiment cap, dense CH indexing,
u32 query/segment aggregate bounds and single-batch submission require explicit
scoped changes before scale. Preserve original submit rejection/unknown-fate,
fence, immutable-generation and completed-picture contracts. A fixed batch per
actual rendered frame must be distinguished from repeated submissions during
one operation.

**Concrete tranche is now released to the sole source author.** Reuse the existing
dense world-registered receiver rectangle and probe min/count, aligned to 32²
mask/dispatch units, with visible footprint+2-cell halo and every requested
offscreen receiver in its bounding rectangle. No sparse page cache/directory.
Missing coverage retains the completed picture; costly disconnected/all-world
requests reject explicitly. Submit at most one safe page quantum per actual
rendered frame, preserving per-batch u32 GPU counters and u64 CPU totals. Publish
only the complete matching immutable window; keep existing rejected/accepted/
unknown submission, cancellation, drain and source-generation lease contracts.

Actual-world selection is a new startup opt-in `--world-lighting=direct-window`
on normal generated frontier/hollows 4096 terrain, with legacy default. The
64×36 showcase remains a control, not a real-world-at-scale result. Source capture
uses the whole authoritative stable grid and exact overlays, not cropped/dirty
lists. Used-model/material and resident/absent chunk census is collected at
runtime; no retained normal-world census currently exists. The first mode uses
explicit black boundary sky. Actor/camera/matter simulation remains; ordinary
terrain/source tools are explicitly disabled with a UI reason until their
lighting revision transaction is attached. Showcase atomic door/lamp edits remain
supported regressions. These restrictions prevent a final shipping claim.

Absorbing ice/crystal are valid descriptor/generation capabilities; the opaque-only
native capture cannot honestly reinterpret them. Minimum homogeneous Beer–Lambert
absorption/emission is therefore brought forward: retain sigma, volume j and
surface S in the compact palette, integrate actual metres in traversal order,
skip only vacuum, include volume emitters in direction bounds and emit opaque
first-hit S once. Reuse canonical existing segment mathematics and analytic
controls. Finite FP32 attenuation may round to zero, with a once-per-source
FLT_MIN-scaled omitted-radiance bound checked against the existing absolute
transport tolerance and a bright-source-behind-thick-medium control. No FP64 or
per-ray certification. Scattering, feedback and particle contributions to world
transport remain open.

Another source-backed blocker requires correct native 4× consumer support before
the actual-world run: current spatial content admission rejects blend terrain
and particles, and the native shading branch assumes opaque cut faces. The
coordinator accepted Astra's GridPass-owned optical-composition tranche, retaining
background→terrain→liquid→solid→gas/disks→actors→Hable ordering. Use the existing
fenced RGBA16F background cache for displaced optical reads plus direct per-sample
background; zero-displacement transparent identity blends over the actual sample.
Preserve authored opacity, texture alpha, mapped normal, IOR and bounded
refraction. Transparent terrain samples incident at its world position; the
opaque key/rim convention stays opaque-only. Existing matter preparation remains,
with sample-frequency variants only for final liquid/solid/disk draws. Program,
root and pipeline selection includes spatial policy independently of signature.
Final field/backdrop readers retain their actual owners in fence-protected slots
before fallible binding; geometry-only roots bind only required material/atlas
inputs. Preserve the current ownership guard. No Renderer/Retro/Hable redesign,
MSAA fallback, material opacity substitution or particle omission.

Validate all four raw samples in the existing shared fixture for actual ice/
crystal, zero-opacity identity, displaced refraction, mixed terrain/matter edges
and old-reader generation/mode changes under ordinary and Core+GPUAV policies.
Three 1920×1080 cached backgrounds estimate 47.5 MiB before overlap; measure added
cache/composite cost separately. Then measure the real generated-world source,
requested set and frame. No actual-world source, media, consumer or performance
acceptance is claimed yet; implementation is in progress.

The native final optical programs use premultiplied composition
`RGB=a*reflection+emittedS`, source blend ONE and destination factor `1-a`.
Authored opacity is preserved; zero/tiny opacity emission needs no division or
unsupported-material workaround. Opaque and legacy-family contracts remain.
The shared raw fixture now contains 6 original + 19 new captures sharing one device,
GridPass, WorldRadiancePass and helper family; the independent Sol test lane
froze its two files and returned integration ownership to the primary source
author. Ordinary/GPUAV execution is still held until the coherent freeze.

The static terrain/per-surface `8H+2S` guard does not bound particle-only emission
or arbitrary overlapping transparent emitters. Aggregate particle HDR safety is
explicitly UNQUALIFIED in this opt-in component tranche. Preserve actual rows,
record count/emission maxima, exercise controlled accumulation and reject observed
overflow/nonfinite values in the measured scene. Do not clamp, omit particles or
infer whole-scene safety from the producer bound. Aggregate admission/pre-exposure
remains required with the later post chain before production-default/shipping
acceptance. This is an open gate, not a change to the agreed final target.

Latest execution steering: preserve the authored tranche and all completed
comparisons. Close its finite source freeze, then build only `engine.tests.asset`
first and run the existing shared spatial_grid ordinary raw-sample leaf as the
earliest discriminator. The next accepted increment is native 4× optical/matter
consumer correctness, not the whole producer. Coherent Astra review may overlap
that focused feedback. On a valid failure, stop expansion and fix only the
demonstrated issue; on success, complete the relevant GPUAV/admission/editor
checks before consumer acceptance. Producer/game builds and actual-world checks
follow as the next increment. No new plan, fixture framework, comparison tuning
or unrelated rebuild is needed to adopt this guidance. Broader delivery gates
remain open and unchanged.

The consumer fixture now has 29 captures in the same shared application/device
and pipeline family. A measured normalized RGB endpoint overshoot
(`0x3f800004`, four FP32 ULP above one) was corrected by decoding byte 255 to
exactly one; the strict BRDF domain guard remains. Five invalid fixture particle
volumes were corrected to the existing per-cell limit, with a matched closer
overlap camera/background baseline preserving the additive-emission assertions.
The ordinary run progressed past those failures but cannot complete the particle
pipeline. A single Core+GPUAV diagnostic exposed a Slang SPIR-V compiler internal
error, `ResolveVaryingInputRef`, in `fragmentParticleSpatial`; leaf 281.61 s,
CTest total 291.07 s, exit 8 within the registered 300 s bound. This is not a
timeout or consumer acceptance. The accepted bounded remedy moves sample
interpolation into the three fragment entry points, following the working terrain
entry, instead of interpolating a varying struct passed into a helper. Preserve
all sample offsets, shading and composition. Run the same ordinary raw29 before
Core+GPUAV; another such ICE stops production remedies and requires a pinned
compiler reproducer. Evidence is in
`direct-world-window/runs/spatial-grid-pipeline-context-validation-diagnostic-20261103/`.
All generated-world, scalable transport, temporal and hardware gates remain open.

The entry-point interpolation remedy cleared the compiler error. Raw30 then
passed the controlled, same-field refraction comparison but failed the mixed
terrain/matter witness: gas was visible on empty samples and occluded on covered
samples; liquid/solid matched the background everywhere. A single all-phase
diagnostic established that result. Source analysis finds the legal single
particle falls below both half-resolution reconstruction thresholds at the old
zoom. A supported closer-view/two-probe correction is authored but unexecuted;
the consumer gate remains failed, with its thresholds unchanged.

Actual-world integration is now built and has one finite frontier4096 smoke run.
The field admitted a 128×128 receiver window (16,384 rows, 16 pages), produced and
presented a matching generation, then the next `applyStagedFrame` rejected missing
optical geometry/source under its resident-world policy/particle guard. No PPM
was saved. This runtime staging failure takes priority over further fixture work.
The startup field-to-publication observation was 33.817 s and publication to
matching present 6.207 s; neither measures normal-play edit age. Initial observed
GPU submissions were 2.618–4.745 ms, not warm-frame qualification. The admission
ledger estimated 1,399,949,344 peak bytes; measured all-live VRAM remains open.
The process exited 1 with 14,031 leaked bytes/23 allocations, of which known
DBus leakage is only a subset; cleanup is not qualified. Evidence is
`direct-world-window/runs/frontier4096-direct-window-smoke-20261103/`.

The bounded Game-only routing correction now passes source review, format/style,
the affected `app.game` build and one actual Frontier dispatch/capture. It uses
the existing retained-terrain route only for a committed matching direct-window
world, preserving live actor/current-particle updates and the Grid ownership
guard. Dirty/stale/missing-view error paths were source-reviewed, not fault-tested.
The actual source census is 4096×4096 cells, 1,024 resident chunks and 47 IDs.
The capture receipt does not record the consumed particle epoch or actual
internal raster/sample count. Matter timing follows the capture; therefore
post-Matter current-particle consumption and internal 1920×1080/4× remain
unproved by this runtime evidence, despite the source-checked route.
The capture is fenced, matching revision 1, 3840×2160; no staged-frame failure or
Vulkan validation error was reported. Exit remains 1 with 14,007 leaked bytes,
including 12,285 bytes outside the known DBus stacks. No cleanup pass is claimed.

**Visual result is REJECTED as the representative target.** At actual display
scale the generated starter room is almost entirely black with no readable
receiving background. The saved frame proves integration, not the requested
environment appearance. Next establish the actual starter-room emitter and
receiver conditions and reuse an authoritative existing source/background
facility for a readable lighting witness; no texture work or ambient substitution.
The unexecuted mixed-fixture correction stays held while this visible blocker is
resolved. Temporal reconstruction, indirect transport, gameplay edits, particle
transport/HDR, cleanup and all agreed hardware gates remain open.
Evidence: `direct-world-window/runs/frontier4096-static-terrain-routing-20261103/`.

One content-only source experiment is complete: reuse existing amber asset as
ID64 in both ordinary palettes, paint four ceiling-attached landing cells, with
canonical textures/pack/shaders unchanged. Both definition-only profile checks
passed (~1.5 s each), then the unchanged binary saved another matching 4K frame.
The actual full-world census confirms exactly four opaque emitting ID64 cells.
**Representative appearance remains REJECTED:** ceiling and a small backwall
region receive warm light, but most of the room is unreadable. This result
authorizes no automatic intensity, EV or source-count sweep. The next decision
must distinguish a receiver/post-chain defect from source reach at room scale.
This is functional source evidence only, not visual, transport or budget
acceptance. The process again exited 1 (14,043 leaked bytes, 12,285 outside DBus).
Evidence: `direct-world-window/runs/frontier4096-starter-amber-20261103/`.

Accepted next discriminator: one unchanged Frontier capture using the existing
final-page fenced coefficient readback, about 1.75 MiB plus its CPU copy. Read
three predeclared empty native receiver centres at landing-local (44.5,29.5),
(42.5,6.5), (66.5,6.5), deriving and verifying their world coordinates against
the authoritative source. Their analytic lamp-rectangle midpoint counts are
45/4/3 of 256. Compare actual DC RGB with count×authored S/256 and report the
corresponding same-capture backwall pixels. No shader, source, EV, material or
geometry variation; no new pipeline or CPU world-lighting reference. A DC match
alone does not qualify the layered-albedo/filter/display response. Keep that
prediction gap and the diagnostic readback cost explicit before choosing the
next transport or consumer change.

The three-point diagnostic is complete. Corrected capture-fence provenance and
the immediate opaque/nonemitting room ring passed independent review. The
affected app build exited 0; the single unchanged Frontier capture saved matching
receipts. DC RGB agrees with all three analytic counts to FP32 precision:
red .439453125/.0390625/.029296875. Corresponding RGB8 pixels are (44,23,0),
(0,0,0), (0,0,0). The PNG/PPM hashes exactly match the earlier amber capture,
so readback did not change rendering. This closes only the three-point direct
producer check; it does not establish scene-wide quality or the consumer's exact
filtered-albedo/display prediction. The next decision must explain the black
pixels or bound the gain of the next lighting contribution, rather than assuming
a weak diffuse bounce alone makes the room readable. No further source/EV/albedo
variation is queued. Evidence:
`direct-world-window/runs/frontier4096-three-point-diagnostic-20261103/`.

Next is a bounded offline consumer check on that retained frame: bound canonical
layer albedo/alpha over the actual convex filter footprint and nearby native DC
variation, then use the existing independent AgX reference at EV3 and sRGB8
quantization. Report code intervals for all three pixels, retaining the nonblack
control. Only centre DC was logged; neighbour bounds derived from the verified
lamp rectangle are predictions, not additional GPU measurements. A loose interval
is inconclusive. No new GPU variation or full CPU renderer is authorized by this
step. First diffuse bounce remains required, with its linear DC gain and final
display-code gain reported separately; it is not assumed to solve readability.

Offline checkpoint result: **INCONCLUSIVE, closed without a rerun.** P1/P2
exceeded the fixed 250,000-reference-evaluation cap. Review withdrew P3's narrow
interval because the script omitted the spatial resolve's axial-neighbour reads
before nearest Retro presentation. The original report is preserved with a
separate `review-verdict.json`; no cap increase, substitute endpoint proof or new
renderer is authorized. The three measured producer rows remain valid; exact
consumer attribution remains open. Evidence is
`direct-world-window/offline-three-point-consumer/execution-20261103/`.
The next concrete playable-world witness should close the previously identified
capture gaps: a frame after real Matter advance with its consumed publication
identity and actual encoded scene extent/sample count, using the existing
capture path. Keep visual rejection, consumer tests, indirect/temporal transport,
cleanup and all product gates explicitly open; do not infer them from that witness.

The playable-capture proof now passes independent source review, the affected
build and one actual Frontier run. Initial publication/joint/Driver tick 0 became
consumed publication 1, joint tick 1 equal to stable Driver tick. Frame 66238,
sequence 1, fence 12298 records device epoch 2, 116,868 particles/13,044 moving
particles and actual 1920×1080/4-sample RGBA16Float scene encoding, with matching
3840×2160 output. No app/Grid failure or VUID was reported. Exit remains 1 with
14,019 leaked bytes; cleanup and normal-play timing remain unqualified.
Evidence: `direct-world-window/runs/frontier4096-playable-capture-proof-20261103/`.
The corrected native optical/matter fixture's ordinary raw30 run also passed
(164.75 s leaf, 172.96 s CTest; one family/three helper programs/three helper
pipelines/one world producer). Its Core+GPUAV gate is still pending.

Core+GPUAV raw30 subsequently passed as well: 242.43 s leaf, 251.83 s CTest,
255.00 s owned wrapper; all 30 captures and shared setup completed, zero reported
VUID/validation errors. Both policies stayed within the registered 300 s limit.
Summary: `direct-world-window/runs/raw30-supported-mixed-validation-summary.json`.
Focused background/admission and Editor scene-flow leaves remain separate
consumer checks; full-world performance and broader delivery gates are not implied.

Next technical decision: one diffuse bounce uses world-wide exposed native-face
RGB32F incident moments, existing normalized diffuse kernel and 256 native direct
directions; no second Lambert division or authored-emission reinjection. Coarse
diffuse reflectance comes from linear tint×filtered mean×(1−metallic). The indirect
RC window includes recursive parent guards and traces C3 to the actual boundary;
offscreen face coverage is complete. Before face-buffer or new kernel work,
preflight the actual source's face/ray counts, lookup/staging/overlap bytes and
guarded interval work. Keep the 512 MiB resource guard and all agreed product
gates. No dense full-native CH allocation or unmeasured scalability claim.

Released preflight scope: measurement-only CPU census in the existing source/pass
for an initial or changed immutable source, skipped for unchanged camera demand.
Count exposed faces by page/orientation and conservatively planned outward rays
with face-expanded page emission bounds. Charge 12 bytes/face, conservative
four-u32/cell lookup, staging/overlap and owned producer storage; label the existing
external reserve as an estimate, not live-device measurement. Report recursively
guarded C1–C3 interval/connector work. No face-buffer allocation, kernel, public
API, shader, CLI, albedo, DDA or source change is part of this first discriminator.

The focused background/admission and Editor scene-flow ordinary checks passed
on the raw30 binary (131.69 s and 129.21 s leaves, no reported VUID/validation
errors). These are bounded consumer results, not full-matrix delivery acceptance.
The CPU preflight then passed independent review and the affected app build.
Review corrections moved observation after direct admission rejection and made
future peak-byte arithmetic checked; rejected all-pages stress no longer triggers
a duplicate census. One actual Frontier preflight is authorized; no new face GPU
work or transport/performance acceptance follows from the CPU forecast alone.

Actual Frontier4096 preflight complete: 526,014 exposed faces (129,875/129,875/
133,132/133,132 by orientation), 10,269 active of 16,384 pages, two emitters,
3,178,607 mask tests and 2,081,849 included page-directions. Outward rays are
67,329,792 unmasked and 53,849,492 conservatively planned after page masks.
Owned ledger 329,877,636 B plus estimated external reserve 1,073,741,824 B yields
modeled future peak 2,235,022,140 B including proposed staging/old-new charges;
overflow false, modeled resource limits pass. This is not measured all-live VRAM.
The logger interval for CPU preflight was 2.984 s; profiler-trace gate is unrun.
One matching post-Matter 4K capture completed; exit 1 retained 14,055 leaked bytes,
including 12,285 not attributed to DBus. No face buffers/kernels were allocated.
Evidence: `direct-world-window/runs/exposed-face-preflight-frontier4096-20261103/`.
The next decision must address the 53.85M-ray refresh work and bounded per-frame
scheduling before a global incident refresh can claim 4 ms/one-second viability.

Decision after preflight: do not implement a global moment refresh. At the current
262,144-query/frame quantum, 53,849,492 rays require at least 206 frames/3.43 s
before reconstruction. Instead discover every opaque face reached by the actual
requested C1–C3/refinement rays and all positive-weight connectors, including
offscreen terminal hits. Reuse native compact traversal and deterministic entered
face ownership. Deduplicate in a generation-scoped GPU bitset/work list, charge
all lookup/capacity/staging/overlap resources, and reject overflow or unresolved
required faces. No persistent face cache or missing-as-black behavior.
First discriminate complete discovery/counts/offscreen coverage and measured work;
then compute the full incident moment for every required face before albedo and
indirect composition. The prior complete field remains published while pending.
Quality and combined GPU/age/storage gates are unchanged; first-bounce/feedback
acceptance is still open. No appearance or consumer-darkness work is queued.

Discovery core is authored and has passed an affected C++ build; Editor/Game
integration is held for the shared GPU controls. Review requires native unit-cell
event ownership at hierarchy block exits: compare both nearby native plane times
with the FP32 exit event and choose the forward cell before opacity/emission/key
lookup. This intentionally repairs accelerated direct results where they disagree
with unit traversal; existing direct quality gates are unchanged. The face-only
draft that would reject a legal diagonal-vacuum case is superseded and unexecuted.
Hierarchy on/off controls cover identical/distinct opaque cells and grazing into
vacuum, checking status, L/T and decoded face. No FP64/epsilon/second tracer.
Other review corrections enforce exact query completion, charge diagnostic CPU
key storage, and create the optional discovery pipeline lazily. The failed build
route's 88 root-owned generated outputs were replaced with byte-identical,
user-owned copies with original files retained; the subsequent C++ build passed
and ownership stayed correct. No source/config/cache purge was used.

The corrected shared core built, then its ordinary GPU leaf stopped in the
existing homogeneous-medium check before discovery controls: sigma=j=2^127 over
one metre returned zero instead of one, status zero. The branch predates the DDA
repair and the failing ray does not use hierarchy skipping. Earlier direct
window/aperture controls still passed. Cause of the numeric lowering remains
unproved; no discovery runtime qualification follows from that failed run.
Approved one local FP32 ratio-conditioning experiment in the nonweak branch:
normal significands/exponents and bounded power-of-two restoration, preserving
weak/zero branches, existing input domain and loss tolerances. Add unequal thick
ratios .75/2.4 beside the equal-one witness. If it fails, stop arithmetic edits
and capture actual intermediate values before choosing another remedy. Editor
and Game integration remain held until the core GPU controls can execute.

### Superseded sequence and coordinator visual assessment

The user reaffirmed this order: close the stone comparison; finish a convincingly
dressed playable bay; measure the bounded seven-pair sparse pipeline reuse;
resolve scalable cascade quality through a tightly bounded comparison; then
integrate and qualify temporal reconstruction, remaining lighting features and
full-world budgets. This ordering supersedes older next-step statements below.

The coordinator visually **accepts rounded-pebble v2 for integration** after the
three actual lamp frames and native-scale crop comparison. Irregular full stone
bodies improve on rejected v1's repeated dots. No further stone recipe tuning is
needed. Durable authoring/provenance and generator integration remain to do;
canonical files have not yet been replaced. The bay itself remains visibly too
sparse for acceptance. Next work is concrete industrial dressing, lit receiving
backgrounds and readable playable space, preserving working door/lamp behavior.

V2 actual-game evidence is under
`service-lamp-switch/runs/v2-rounded-pebble-candidate-20261103/` in the diagnostic
root. OFF/ON age was 1341.952/1355.779 ms, both over the unchanged one-second
gate. Producer observations 6.826/6.222/5.909 ms do not qualify 4 ms. There were
no reported VUID/validation errors; exit 1 retains the known DBus LSan class.
The accepted pipeline reuse design remains queued after bay dressing. No claim
of a measured reuse improvement or scalable cascade acceptance is made.

**Decisive recommendation:** adopt native-cell world transport plus directional
probe storage and render-side fine detail. Retire exact Bézier intersection,
certification and FP64 refinement from the candidate frame path. Do not copy
v22 verbatim or promote the failed footprint adaptation. Use the qualified tiny
straight-ray control to judge the playable art and shadows, then choose scalable
cascade reconstruction from measured comparisons. This is a bounded experiment,
not a replacement of the full-world performance target with direct tracing.

Coordinator decision for goal continuation: implement only the bounded candidate
experiment below. Its coarse transport reference is native-cell occupancy, with
the existing meaningful wall/door/emitter masks and unchanged 15%/1% limits.
Fine rendered contours receive a separate final-image check; no border band is
exempted. This explicitly changes the coarse reference from Bézier intersections
without granting production or full-world acceptance. The retained game/renderer
foundation is compiled and the small field now lights actual foreground and
backwall surfaces. Do not extend the retired exact-view architecture.

**Current bounded sequence:** the under-ledge lamp is accepted in a ready 4K
frame and the uncaptured timing window completed. The real-game door check now
completed blocking, traversal, occupied-door rejection, final closure and a
matched closed/open image pair with a lit receiving backwall. The process still
fails shutdown LeakSanitizer on the existing libdbus allocation class.
The bounded screen-refinement comparison passed functional validation but
introduced visible stepping and a rectangular transition; keep it off. The
pale-stone comparisons and warm-edit phase attribution are now recorded below.
Sealed texture inventory reuse passed the shared ordinary/GPUAV fixture and the
actual door witness; it removed the repeated image-upload traversal. Receipt age
still exceeds one second, and the current artwork remains far from the reference.
The affected existing Grid lifecycle checks also passed. Native directional rough
specular and a physically scaled pipe-normal correction are now integrated and
checked in the playable scene. Six-control MSAA validation shares its helper
pipelines. The stone trial remains a comparison asset, and the attempted warm
timing pair is incomplete. Both the fixed 16-phase cascade comparison and matched
terminal-C1 junction comparison failed their brightness gates. Endpoint alignment
alone is rejected; no full-stack trial or more phase tuning is authorized. The
independent Vulkan resolve-state defect is corrected and validated. Improve the remaining material/dressing, qualify motion
and costs, and attribute the remaining pre-stage delay.
The service-bay lamp switch now completes ON→OFF→ON in the actual game, including
the receiving backwall and tread. Both measured transitions still exceed one
second; the next latency investigation must attribute that remaining delay.
The current priority is the reference-art gap: one new rounded pale-stone
material trial at verified game scale. Canonical sandstone uses 128² maps over
a 2 m repeat. Author a seeded periodic convex-stone field with physically
consistent exported height/normals and a 512² trial; establish its exact scale
and channel checks before baking. Reuse the existing procedural-PBR exporters,
seed17/17/29 verification, exported-map relights and isolated runtime root. The
previous faceted v4 remains an unpromoted comparison. No runtime assets, pack
entries, shaders or geometry change before offline review and an actual same-scene
comparison. This art work does not close the outstanding transport, latency or
display gates.
The material trial's initial 16-sample quad calibration failed constant-offset
registration; retained-map requalification passed texel ownership but failed
the unchanged 4% physical slope residual at 4.1494%. Its normal-only control
passed. Both failures remain archived. A proposed 64-sample trial was frozen
but never executed. New evidence from the installed Blender commit's
[bake sampler](https://github.com/blender/blender/blob/e13b728c6880/intern/cycles/kernel/integrator/init_from_bake.h)
supports replacing that trial with one single-sample data triangle
covering the entire tile. Calibration, the attribute atlas and all material maps
must use the same fixture; preview rendering keeps its own sample count. Verify
the pinned [rasterizer's](https://github.com/blender/blender/blob/e13b728c6880/source/blender/render/intern/bake.cc)
predicted registration and the same known-slope,
2%-gain/4%-residual, coverage and negative controls before material qualification.
There is no threshold relaxation, normal gain adjustment or runtime axis flip.
That point-data calibration qualified after recovering a JSON report-generation
failure from its retained maps, without rebaking. Three 512² seed17/17/29 exports
then passed fixed QA: repeat identity, seed variation, channel/seam checks and
physical normal gains around 1.0013 with approximately 0.14% relative residual.
All six exported-map relights were inspected. The single actual isolated game
comparison completed ON→OFF→ON, with backwall/tread holdouts byte-identical to
canonical frames. The coordinator inspected all three frames and native-pixel
crops and **rejected the appearance**: regular embossed dots do not match natural
stone. No canonical material or descriptor was promoted. Preserve the qualified
pipeline and rejected trial. The next material change must disrupt row-like
placement and broaden pigment across stone bodies while preserving the physical
height/normal and joint-colour contracts. Actual evidence is under
`service-lamp-switch/runs/rounded-pebble-candidate-20261103/`; recipe/QA and visual
review are under `material-lookdev/rounded-pebble-authoring/` in the diagnostic
root. Latency remains above one second and the known shutdown leak remains.
The coordinator accepted a bounded v2 source revision: centre jitter increases
from ±10 mm to ±33.333 mm (40% of lattice spacing), and pigment remains full
through ellipse radius-squared q=.70 before smoothly tapering to zero at q=1.
Only pigment composition uses that new footprint; the quartic height and exact
analytic normal stay unchanged. Preserve the joint M/J/albedo relationship and
all numerical gates, including isolated-cap coverage despite increased overlap.
The modified .blend route was rejected because it would require weakening the
current exact source guard and fragile rewiring of unnamed Math nodes. Use fresh
v2 source identity and point calibration, then the same fixed three-seed QA and
six-proof visual review before one actual comparison. V1 remains immutable and
rejected; no parameter sweep or canonical promotion is authorized. V2 source
is frozen and unexecuted at
`material-lookdev/rounded-pebble-authoring/v2/source-freeze/manifest.json`
(SHA-256 `376891933d98a9e22a04f68caa92415ae839fcf68ae7547e50707488c8fb9b14`).
Next: review that bounded delta and run one fresh calibration; only a PASS admits
the fixed three-material-export sequence. No v2 visual result is yet claimed.
V2 source review and its fresh point-data calibration now passed (report
`v2/calibration/point-data/calibration.json`, SHA-256
`930c4e9c5ed9d5dfab280081e9920d964e3ec5872d1bab140e705209442f10e0`).
The fixed 512² seed17/17/29 batch is in progress; no numerical or in-game v2
acceptance is claimed. The earlier missing-log-directory launch failed before
Blender and remains separate from the successful calibration process.
Keep the tiny direct control for these comparisons. Select scalable cascades
using quality/work evidence before expanding world coverage. No full CPU cascade,
new viewer or broad renderer rewrite precedes this experiment.

In parallel with source review, one offline rounded pale-stone material trial
uses the procedural-PBR skill and Blender 5.2.3 LTS on CPU. Its editable seeded
recipe, export contracts and commands are under
`out/diagnostics/environment-lighting-20261102/v22-reassessment/material-lookdev/sandstone-authoring/`.
The trial checks seeds 17/17/29 at 512² and relights only the exported maps.
No runtime assets or pack descriptors change before decoded-map, seam, normal
orientation and visual checks. Current Grid supports albedo/normal maps and
scalar roughness/metallic; height and roughness maps remain offline. Native world
rough specular is now implemented as recorded below. This material trial does not establish
consumer compatibility or complete the art target.

Both offline stone trials completed their seed17/17/29 export checks on Linux
with Blender 5.2.3 LTS, in 58.221 and 57.876 seconds respectively. Decoded repeat
identity, seed variation, channel/metadata, normal and seam smoke checks passed.
The coordinator inspected all six relit previews per trial. The first trial's
14/6 mm grains read too fine; the 35/14 mm, 6 mm-relief variant is accepted only
as a candidate for an in-game A/B. Its review is
`material-lookdev/sandstone-coarse-authoring/VISUAL_REVIEW.md` under the diagnostic
root above. Actual UV scale, import and tangent orientation remain unqualified;
no runtime material or pack entry changed.

The following experiment history records rejected candidates as well as the
accepted small-scene control. For the current integration state and next action,
see “Industrial bay checkpoint and bounded correction” below.

## Bounded experiment history

### Current solver decision: fixed 16-phase coverage comparison

Return to scalable world transport after the material checkpoint. Astra's accepted
next diagnostic keeps the current four-level cascade, native-cell refined probes,
footprint geometry, merges and source masks unchanged. For each unchanged open
and closed aperture, run phases `(k+0.5)/16`, `k=0..15`, in the same existing GPU
fixture. C1 has 16 directions, so the union of its **nominal** directions is exactly
the passing straight control's 256 midpoint angles. Upper-level sampling and
actual connector directions still differ; this is not v22 equivalence.

Read each completed valid generation, record status/work and per-phase signed
bias, and average raw DC per queried point in double. Release each generation
after its observations. Compare all twelve means per state against straight-256
and the independent straight1024 reference; keep the existing point masks,
denominators, 15% L1 and 1% blocked-side limits. Collect both states before the
final physical assertion. Numerical failures must be reported separately and
never averaged away. No best-phase selection, clamping, CPU cascade, production
history, extra pipeline/application or playable transport change is authorized.

If the average qualifies while straight-256 still passes, a bounded actual
temporal scheduling/edit-convergence experiment becomes worth measuring. Static
equal-weight averaging alone proves neither one-second freshness nor moving-world
quality. If either physical gate fails, stop phase tuning. The next decision must
isolate the C1 junction with a terminal far record and connectors reaching its
actual start; it cannot presume that junction is the sole cause. The current
footprint intersection generally differs from the stored far start `p+4d`, and
upper merges share a connector across four angular children.

The existing fixture's source-derived query admission bound is 18,752 per phase,
600,064 across both states; verify actual counters/demand rather than treating
those estimates as measurements. Serial execution does not authorize new scratch
reuse or lifetime rules: report the existing planner's actual overlapping peak.
At 4096² the current refinement alone is estimated at roughly 1.07 billion
connectors **per phase**. Full-world storage, work scheduling, compact signed CH,
matter/media/feedback, 4 ms, 1 s and 4K60 remain open regardless of this outcome.

**Fixed-phase result: rejected.** The ordinary shared fixture completed all 32
generations in one process. Each phase attempted and completed 18,752 queries,
with zero failed queries or invalid rows and all 3,896 expected rows written.
The 600,064-query total matches the admission bound. Raw phase averaging produced
65.3312% open-point relative L1 (limit 15%); direct reference sum was 0.236328125,
averaged cascade sum 0.1132278442, absolute error 0.1543960571 and signed bias
−0.1231002808. Closed-side radiance stayed zero. The unchanged straight-256 control
still passed at 5.78512% L1, with zero matched-direction traversal disagreement.
Dense nominal angular coverage did not qualify this cascade. Stop phase tuning;
the next bounded decision isolates the final near/far junction rather than
introducing exact geometry. This is not a result for literal v22.

Observed admission peaks were 2,093,440 bytes for phase revision 1000 and at most
1,729,324 bytes for subsequent phase revisions; these are the existing planner's
small-fixture accounting, not measured full-world VRAM or a new reuse contract.
CTest selected one leaf and exited 8 in 15.83 seconds on the final brightness
assertion. Genuine Vulkan layout VUIDs also identify the shared screen-contact
background attachment; this run is **not validation-clean**. Their independent
fixture cause must be resolved before another native comparison. Expected
negative-path logger output must not be mistaken for teardown failure. GPUAV and
retries remain held. Evidence and frozen source are under
`cascade-phase16-coverage/source/dc-validity-fix/` in the diagnostic root above.

**Next bounded decision:** first correct the independently diagnosed Vulkan
dynamic-rendering attachment resolve transition through the existing checked-in,
hash-verified Slang-RHI build-tree patch. Its resolve attachment declares
`COLOR_ATTACHMENT_OPTIMAL`, but the pinned backend requests `ResolveDestination`,
which maps to a transfer layout/stage/access. Request `RenderTarget` only at that
attachment site; retain explicit transfer resolves, D3D and real resolve pixel
checks. Adding transfer usage alone cannot repair the layout mismatch. Verify
idempotent patch application and both reported VUIDs' absence under active layers.

Backend correction checkpoint: both configurations succeeded with stable generated
output and unchanged cached upstream source/test settings. The exact MSAA
clear/resolve pixel leaf passed with Core and GPU-assisted validation enabled
(3.88 seconds). The shared lighting leaf then reported zero VUID/Validation Error
matches, retaining its expected 65.3312% brightness assertion failure. The Vulkan
layout defect is resolved on these Linux runs; the rejected cascade is unchanged.
Source and execution evidence are under `vulkan-dynamic-resolve-fix/` in the
diagnostic root. Windows compatibility and other runtime paths are not established
by these checks.

After that independent validation fix, the next solver experiment is a matched
terminal-C1 A/B using the same two states, twelve points, sixteen phases and four
parent weights. For each tuple, share one terminal far trace beginning at `p+4d`.
Arm A uses the current footprint-boundary near endpoint; arm B ends its near
connector at the actual far start `p+4d`. Reuse the real GPU trace, connector and
composition helpers in the existing qualification fixture; do not build a CPU
cascade. Use the producer's actual terminal far bound **56**, derived from the
padded C1 lattice, rather than the straight control's bound 44. There are an
estimated 24,576 tuples, 49,152 paired interval outputs and at most 73,728 traces. Preserve
raw DC averaging, status rejection and unchanged 15%/1% physical thresholds.
If B alone passes, admit one full-stack junction comparison. If both pass,
investigate upper merging before changing the refined junction. If B fails,
reject endpoint alignment alone as a remedy. A joined, bent path still differs
from a straight ray; this experiment neither promises a fix nor qualifies
full-world scheduling or storage. Source implementation of this bounded comparison
is now authorized after backend validation. Explicitly retire the completed failed
phase16 helper and assertion from active execution while retaining its frozen
source/logs and rejected result above. This avoids repeating the known failed
600,064-query experiment; it does not qualify the rejected full stack. The new B
arm must meet the unchanged physical gates or fail the leaf. Mandatory straight,
analytic and lifetime controls remain active. A B pass qualifies this terminal-C1
experiment only, and authorizes neither production integration nor a performance
claim. Native execution waits for the new source freeze and review.

**Matched junction result: B rejected.** Source review, touched formatting/style,
post-format author audit and the affected `engine.tests.app` build passed. One
ordinary registered leaf completed in 16.74 seconds with zero VUID/Validation
Error messages. Both arms had valid status for all 24,576 tuples, 49,152 paired
interval rows and 73,728 completed helper traces, with zero failed queries.
The current footprint arm A had **55.2169%** relative L1; aligned-start arm B had
**33.6260%**, both above the unchanged 15% limit. Both closed leakage gates passed,
and the straight-256 control remained passing. B improved this particular terminal
comparison but did not qualify. Reject endpoint alignment alone as the remedy;
do not promote it, run the full stack, tune phases or return to exact geometry.
GPUAV and retries were held after the physical failure. This does not measure
literal v22 or invalidate the coarse-world/fine-render split. Source and complete
execution evidence are under `terminal-c1-junction/` in the diagnostic root.

**Next goal-facing checkpoint:** implement an actual service-bay under-ledge
amber lamp switch. Retain its silhouette/collider and fixed camera/exposure,
switching only between its authored emitting state and an otherwise identical
opaque non-emitting state. The tread and receiving backwall should lose the warm
pool while the cyan tank stays lit. Reuse the Driver transaction and immutable
Grid/field publication path, including cancellation and unknown-fate ownership.
Capture both actual rendered states and measure input-to-first-matching-present
for each transition. The last approximately 1.27-second door age remains above
the one-second target. No new pack entry, generic editing framework or scalable
RC claim is authorized by this checkpoint; first establish the smallest existing
representation and transaction extension through read-only design.

Coordinator accepted the bounded implementation after read-only design. The
Driver owns enabled state; a copied candidate-local visual row changes only the
dedicated amber material's emissive strength (authored 2.5 or zero), after checking
that ID occurs only at the four authored cells. Build the field source from that
same projected row and preserve the projection after commit. Extend the existing
single service-bay transaction with a typed door/lamp target, retaining its
revision, quiescence, activation/commit, cancellation, Matter binding and unknown
submission rules. Identical chunks initially use the existing conservative
collider cohort. Preserve texture backing/inventory and verify actual reuse.

Use one panel button disabled during a pending bay transaction. The authorized
showcase-only lamp-check CLI reuses the existing coordinator/capture path for
ON→OFF→ON, fixed actor/camera and matching revisions, with the existing 180-second
operational ceiling and mutually exclusive diagnostic modes. Record intent
admission to first clean matching-present receipt before capture readback; this
is programmed interaction, not native input or display scanout latency. Reuse
existing CPU and service-door shared GPU tests for lamp state, source/presentation
identity, retained old state, cancellation, unchanged cells/other material rows
and ownership.

**Lamp checkpoint: functionally complete, latency failed.** Independent source
review (including real movement from spawn onto a qualified interior tread),
touched formatting/style and the post-format author audit passed. Both affected
game targets built. The three exact CPU leaves passed together in one process
(11.13 seconds); the existing shared service-door/lamp GPU fixture passed with
Core and GPU-assisted validation requested ON (16.74 seconds), with no reported
Vulkan validation errors. No extra GPU application or pipeline family was added.

The actual 3840×2160 VSync run completed all three fenced captures and its final
functional marker. The coordinator viewed every lossless PNG: switching OFF
removes the warm pool from the receiving backwall and tread; restoring ON brings
it back, while cyan/violet emitters remain lit and the actor/camera stay matched.
This qualifies the bounded lamp behavior, not the reference artwork. Assets and
shader identity remain canonical; the game uses the tiny straight-256 control.
Programmed-intent-to-first-matching-present receipt was **1342.862 ms OFF** and
**1357.223 ms restored ON**, both failing the unchanged 1000 ms gate. Timings end
before capture readback and do not measure native-input or scanout latency.
The process exited 1 on the known unsuppressed libdbus LeakSanitizer class
(1566 bytes, three allocations) after functional completion. No retry occurred.
Source, tests, actual images, receipts and visual review are under
`service-lamp-switch/` in the diagnostic root. Full-world RC, temporal display,
resource/performance, reference-art and cross-host gates remain open.

Final lamp evidence: revisions 1/2/3 completed at capture fences 707/766/824 with
the same actor pose. Revisions 2/3 each reused all 138 images with zero new
uploads/bytes. The amber backwall and tread RGB-code ROIs change on OFF and
restore byte-identically on ON; the selected cyan interior ROI is identical
across all states. These are descriptive final-image checks, not radiometric
acceptance thresholds. Zero runtime VUID/Validation Error messages were observed.
The direct-reference field remained 589,824 queries, 2,304 rows and 258,048 bytes,
with zero coarse dispatches. Producer GPU elapsed was **8.221/8.854 ms** for the
OFF/restored-ON generations (including statistics copy, excluding CPU preparation
and source uploads); this does not meet or establish the 4 ms lighting target.
Field-begin-to-publication was **1.319/1.332 seconds**, separate from the longer
intent-to-present measurements. Full evidence is
`service-lamp-switch/runs/actual-lamp-check-20261103/final-evidence.json`.

Read-only attribution of that retained run found seven uncached
`pbmpm_sparse_grid` entry compilations in each inferred transition window,
totalling 283.7/295.2 ms of the logger's entry-creation durations. Their seven
Vulkan compute pipeline calls totalled only 0.7 ms per window. GridPass itself
initialized once. Earliest recorded Grid staging begins 24.613/27.680 seconds;
entry creation ends about 28 ms before those events. Intent times are inferred
from reported receipt ages, not separately logged phase markers. These timings
do not account for the entire 1.343/1.357-second latency or prove the remaining
cause. Preserve the distinction between module loading, entry compilation,
native pipeline creation and waits before proposing reuse. No performance edit
or extra native run follows from this observation alone. See
`service-lamp-switch/runs/actual-lamp-check-20261103/lamp-phase-attribution.md`.

**Accepted next latency change, held until the current art comparison ends:**
reuse the seven existing owning program/pipeline `ComPtr` pairs from the current
solver generation when preparing its replacement. Solver initialization already
publishes a valid empty bootstrap generation; only that generation needs cold
compilation. The fixed sparse program descriptors do not specialize on capacity,
material rows or profile. Fresh constants/roots/buffers bind separately. No global
cache, separate family allocation or new shared-pointer lifetime is needed.

Keep standalone sparse initialization and its cold path. A private solver-only
replacement path must reject missing/partial/foreign-device donors, share only
the immutable pairs, allocate fresh mutable resources, and publish only complete
success. Preserve cancellation drain, replacement fences, unknown-submit
retention and buffer-byte accounting. Source scope is the existing sparse and
solver implementation/interface plus its map and focused existing GPU coverage.
Verify bootstrap, cancellation, capacity change, publication and a subsequent
nonempty sparse tick in one fixture; retain standalone lifetime coverage and run
ordinary/GPUAV checks. Emit one structured cold/reuse record per preparation.
Use an existing explicitly supplied observation clock for CPU elapsed if available;
otherwise report counts without introducing an implicit timer. Only a matched
actual lamp run can establish update-age improvement. No GPU-time saving or
attribution of the remaining approximately 0.9 seconds is claimed. Production
edits/builds are held so the in-flight material comparison retains its binary.

A new scalable RC candidate requires a separate bounded design after these
rejections. It must state what directional/visibility information its merge
preserves across a one-cell opening, predict a distinguishing result on the same
24 points, and account for overlap memory and update work. Retain 15%/1% gates.
Neither rejected remedy identifies a unique remaining cause. Keep exact geometry
out of the new playable path and avoid an open-ended repair of this reconstruction.

Implementation checkpoint: the separate native-cell producer, signed FP32 CH
shader and shared GPU fixture are source-frozen in
`out/diagnostics/environment-lighting-20261102/v22-reassessment/production-apply/freeze.json`.
The fixture shares expensive setup across analytic, boundary, aperture, normal
and lifetime checks; ordinary and GPUAV policies remain separate processes.
The candidate builds and has run once under ordinary validation policy. Its
constant/DC/normal controls passed, but the unchanged aperture gate failed:
open-room relative L1 was **92.3295%** (limit 15%); all twelve closed-room samples
were zero. Open reference/candidate sums were 0.236328125/0.0230712890625.
This is evidence about this adaptation, not a measurement of literal v22 or the
old 69.45% failure. GPUAV was held after the physical failure. See
`out/diagnostics/environment-lighting-20261102/v22-reassessment/production-apply/world-radiance-candidate-ordinary.log`.
Grid/background integration and screen refinement remain held pending a bounded
diagnostic decision; full-world resource/performance qualification is unrun.

Coordinator follow-up: test refined probes at **native-cell centres**, with
explicit spacing metadata, before changing intervals or tracing precision.
The spacing-2 decoder has a representational lower bound: at x=7.5 the direct
values for y=3.5,4.5,5.5 are (0,0.06640625,0). Nonnegative bilinear probe DC
on the current lattice makes the first and third reconstructed values sum to
at least the middle. Those three samples alone therefore require at least
0.06640625 absolute error, exceeding the entire 12-point allowance of
0.15×0.236328125=0.03544921875. No phase or interval tuning can qualify that
decoder against these unchanged samples.

The bounded follow-up keeps coarse spacing, phase, four-cell footprint radius,
intervals and CH unchanged. Run spacing-1 with the full cascade stack first.
Only if it fails, collect a terminal-C1 (`cascade_count=2`) control in the same
fixture before the final full-candidate assertion. That control changes both
upper angular quadrature and merging; it cannot independently attribute error
to either, nor qualify a failed full candidate. Both failures stop Grid wiring.
Two full-world spacing-1 FP32 CH fields plus two approximately full-map FP32
parity workspaces are estimated at 4.5 GiB before inputs, exceeding the target.
Signed 64-byte CH would reduce that subtotal to about 3 GiB; padded capacities,
overlap, input storage and timings remain unqualified. This small control does
not authorize full-world FP32 allocation or change any resource/quality gate.

Follow-up result: native spacing-1 **also failed**. Full four-cascade relative
L1 was 56.8698% (candidate sum 0.1253662109375); the terminal-C1 control was
69.4215% (sum 0.095703125), against reference sum 0.236328125. Closed-side
samples remained zero in both runs. Constant controls passed. The same ordinary
fixture completed in 8.59 seconds, including setup and both comparisons; that
wall time is not GPU lighting latency. GPUAV and Grid wiring were held as agreed.
Evidence: `out/diagnostics/environment-lighting-20261102/v22-reassessment/native1-control/ordinary.log`.
The decoder's spatial resolution was one problem, but these results do not
attribute the remaining error solely to upper cascade merges. Do not tune phase
or intersection precision against this failure or call either control qualified.

Next coordinator-authorized checkpoint: **one fixed 256-direction straight-ray
GPU reference mode at native-cell centres**. Stop footprint/phase tuning for
this checkpoint. Reuse the native DDA, immutable source, signed CH, generation
fences and shared fixture. Directions are uniform midpoints fixed in advance;
trace to the first opaque hit or world escape. Compare all existing points with
the unchanged 1024-ray reference and separately with the existing direct CPU
slab oracle at the same 256 directions. This separates angular sampling error
from GPU traversal disagreement without another CPU cascade.

If the unchanged 15%/1% gates pass, run GPUAV once, then integrate this explicitly
labelled **small-scene reference mode** into the playable cavern: lit backwall,
door shadow, moved emitter and offscreen reveal at fixed exposure. Incident light
for rock must be sampled from the exposed side; a probe inside opaque stone
returns its own emission, not its exterior illumination. If the control fails,
report the matched-direction comparison and hold presentation wiring.

A 64×36 update requires 589,824 straight rays before traversal steps. This is a
work-count estimate, not measured GPU time. This bounded mode provides an art
and gameplay baseline; it does not replace or qualify scalable world-space RC.
Full-world transport, compact storage, media, feedback, reconstruction and all
RTX 2080 resource/performance gates remain required and open.

Reference-mode checkpoint: straight-256 passed ordinary (9.02 seconds wall time)
and Vulkan GPUAV (16.15 seconds). Open reference1024 sum was 0.236328125;
slab256 and GPU sums were both 0.234375. Relative L1 was **5.7851%**, with zero
matched-direction traversal disagreement across all 24 open/closed samples and
zero closed-side leakage. Constant controls passed. Source and staged shader
were SHA-256 `057a507d4be532cbdf1fad06cbf1a0a1954792276f4b9413a1438e562e185cb2`.
Evidence: `out/diagnostics/environment-lighting-20261102/v22-reassessment/straight-256-control/{ordinary.log,gpuav.log}`.
These wall times include setup; they are not GPU lighting timings.

Coordinator decision: proceed with the explicit small-scene reference backend
in the actual showcase. Reuse the ready world field across camera motion and
bind its directional coefficients to foreground and backwall shading. The
backwall receiving plane is the authored native crop [0,64]×[0,36]; outside is
transparent distant void, not a fabricated wall. Verify the simulation exterior
is vacuum before admitting the cropped source. Preserve matching source/grid
cohorts, material projection and fenced draw leases. Keep opaque self-emission
separate from exposed-side incident lighting. Initial integration establishes
static startup, pan/zoom and a visibly lit background; Driver-owned door/emitter
edits and final screen-detail comparisons follow as explicit checkpoints.

### Actual playable checkpoint and next bounded change

The actual game now renders the reference-mode field onto both foreground and
the authored backwall. The first owned-window capture is
[3840×2160 playable frame](../../out/diagnostics/environment-lighting-20261102/v22-reassessment/initial-playable-window/descriptor-draw-repair/run/fresh-build/windowed-showcase/first-scene-owned-window.png).
It establishes rendering, **not visual acceptance**: most stone interiors and
the backwall are too dark to read; the violet and cyan inserts look flat. Native
input, pan/zoom, door edits, emitter moves and final frame cost are not qualified.

The latest ordinary/GPUAV shared candidate runs passed in 11.40/20.49 seconds
including setup. The unchanged physical result remains 5.7851% relative L1 and
zero closed-side leakage. Production Triangle payload binding, HDR 4× MSAA and
Retro→Grid lifecycle/pixel regressions also passed in one shared asset process
with Core validation and GPUAV; no VUID or validation error was reported. These
are correctness results, not 4 ms or 60 FPS measurements.

Startup fixes preserve the original vendor push-constant implementation: vertex
draw data now uses a reflected descriptor-backed constant buffer. Native CH
reflection, explicit viewport/scissor state, matching immutable source/draw
cohorts and fenced generation leases are checked. Presentation generations must
qualify a finite response ceiling of 65504 before binding to RGBA16F; the FP32
control keeps its larger domain.

The first runtime reported 589,824 queries, 2,304 refined rows and 258,048 field
bytes, with no cascade dispatches. These describe only the 64×36 reference
backend; they do not measure peak device memory or GPU elapsed time. The game
exited with status 1 after its scheduled exit; its cause remains unresolved.
Its journal contains 44,512 zero-group dispatch warnings and no preserved
application error. The next owned run must preserve direct stdout/stderr and
unit exit metadata instead of relying only on the journal.

Coordinator accepts Astra's next matched presentation experiment:

- Give this showcase a separate, muted stone backwall albedo, initially around
  0.18 linear reflectance, retaining strata without baked pools of darkness.
  Keep backwall response as albedo multiplied by world incident lighting.
- Add an explicit weak front light **only to rendered opaque cut faces**:
  neutral `0.04 × max(dot(mapped_normal, normalize((-0.4, 0.5, 1))), 0)`.
  Multiply by the existing albedo; add own emission once. Apply the term across
  the cut face, including its rim. It never enters background shading, world
  queries, gameplay, feedback or the physical quality masks.
- Compare backwall-only and backwall-plus-front-light at the original camera,
  exposure EV3 and source strengths. Do not increase exposure to hide black
  surfaces or expand nearest-empty-cell sampling through solid terrain.

The accepted `8H + 2S ≤ 65504` presentation admission also bounds the new
nonnegative per-channel response `3H + S + 0.04 ≤ 32752.04`. This fixed render
light changes the presentation model, not the meaning of the 15%/1% world-field
gates. Separate final-image checks must expose that distinction. No full-world
rewrite, broader tracing precision work or new CPU lighting pipeline is granted
by this checkpoint.

The presentation delta is source-frozen in
`out/diagnostics/environment-lighting-20261102/v22-reassessment/initial-playable-window/presentation-experiment/freeze.json`.
Independent Astra source review passed: the mapped-normal term is confined to
native solid shading, emission remains single-counted, only the showcase selects
the new backwall, and linear albedo is encoded/decoded as sRGB consistently.
The isolated capture shaders differ only in the `0` versus `0.04` literal.
The four 512×512 bitmaps were generated and staged byte-identically; decoded
linear channel means range from 0.167 to 0.187. Ordinary/GPUAV candidate runs
passed in 9.74/10.62 seconds. This fixture initializes the actual Grid shader but
does not draw a cut-face material pixel; it cannot qualify the art response.

The first backwall-only game run admitted the field and published the game, but
its screenshot was entirely black, including UI, and is invalid capture
evidence. Its direct stderr establishes a separate shutdown failure:
LeakSanitizer reports 1,854 bytes allocated in `libdbus-1.so.3`; unit exit is 1.
No VUID/validation error was reported. This explains this run, not necessarily
the earlier run whose terminal diagnostics are missing. Sanitizers and existing
suppressions remain unchanged. Preserve this failure and obtain a valid matched
capture without claiming either clean shutdown or visual acceptance.
Evidence: `presentation-experiment/runs/backwall-only/run-record.json` beneath
the diagnostic checkpoint directory above.

One bounded control retry also produced an entirely black owned-window capture
and the same libdbus leak report. ImageMagick failed even with the preserved
explicit argument list; Spectacle's result is byte-identical to the first black
image. **External-window capture is blocked; no matched image comparison has
passed.** The canonical 0.04 runtime was not launched. Preserve
`presentation-experiment/runs/backwall-only-repeat/run-record.json`; do not keep
relaunching this capture route or infer material quality from these black files.
The next capture decision should use the existing RHI readback and actual game
frame boundary, with explicit completion/lifetime handling, rather than a new
viewer or the retired headless exact-lighting capture path.

Coordinator/Astra capture decision: add a default-off, showcase-only final-frame
readback. Validate CopySource surface support at configuration. Record one
texture-to-buffer copy after every scene/Retro/native-UI surface pass in the
same command encoder and submission. Retain one Renderer-owned staging slot
through definite-unsubmitted, unknown-submit and accepted-real-fence states.
Settle presentation exactly once, then publish owned CPU pixels only after
successful presentation and proved completion/map/copy/unmap. Unknown work stays
quarantined until fence, idle or confirmed-removal proof; it must survive the
existing unknown-quiescence shutdown guard. Do not use `Device::readTexture`:
its separate, unfenced submission has failure lifetime outside this frame fence.

The game owns the one-shot PPM file policy and writes already display-encoded
RGB bytes, swizzling BGRA where needed. The flag does not create a new viewer,
lighting pipeline or transport queue. A 4K capture has roughly 33.2 MB of GPU
staging and another 33.2 MB of CPU pixel storage, plus output and transient
overhead; these are estimates, opt-in diagnostic costs, not normal-play timings
or lighting-budget qualification. Source is now frozen in
`initial-playable-window/interactive-frame-readback/freeze.json` under the
diagnostic root: 12 C++ files and four maps, with exact before/after copies and
an incremental patch. Review fixes and execution results are recorded below;
actual 4K image inspection remains open.
The focused tests reuse the existing Editor harness for final scene/UI pixels,
one-shot suppression, stale pre-acquire rejection and capability persistence.
Unknown-submit/present/wait/map/unmap ownership is source-review coverage; no
new injected GPU failure run is claimed.

Readback validation checkpoint: formatting/style passed, and independent Astra
review closed two findings: published CPU padding is now zero, and proven
pre-acquire transients preserve the armed capture. The copy layout uses at least
256-byte valid row alignment, allowing a real padded-row regression on Vulkan;
the 3840-pixel row is already aligned, so its estimated storage is unchanged.
All four affected targets (`app.game`, `engine.tests.app`, `engine.tests.asset`,
`game.tests`) built with durable exit-code-zero records in `build-records/`.
The 130-result transport/wrap leaf passed in 0.364 seconds, and the showcase CLI
leaf passed in 0.164 seconds. Its actual selector is
`game/game_config/lighting_showcase_is_explicit_interactive_and_never_auto_qualified`.

The first ordinary Editor execution failed because its retry supplied an empty
catalog after CPU staging had already succeeded on the busy frame. The fixture
now asserts that earlier staging and explicitly retains its installed cohort;
production guards and all pixel assertions remain intact. The corrected
`asset/render/quarantine/editor_scene_flow` passed in 99.014 seconds, including
setup. It checks scene/UI pixels, a real padded row, busy admission followed by
one capture and duplicate suppression. Core/GPUAV also passed in 116.792 seconds,
with the real layers enabled and no reported VUID/validation errors. Preserve
both failed and corrected records beneath `interactive-frame-readback/`; these
setup-inclusive wall times are not GPU lighting latency.

The first direct game readback succeeded at 3840×2160, BGRA8UnormSrgb, pitch
15360, fence 630, with 33,177,600 bytes each of GPU staging and published CPU
storage. Its PPM-to-PNG conversion preserved every RGB byte. However, visual
inspection found the “Generating terrain” modal and a misplaced view. This is
**ineligible startup-frame evidence**, not the matched playable comparison:
`presentation-experiment/runs/readback-backwall-only/final-frame.png`. The
canonical 0.04 run was held. Shutdown again reported the unchanged libdbus leak.

Source diagnosis established two independent causes: first-world presentation
armed capture before progress-modal retirement, and showcase framing passed the
scene centre to the corner-based Pan controller under full simulation bounds.
The bounded fix is frozen in `interactive-frame-readback/playable-readiness/`.
Game now owns per-frame eligibility, cleared on every update and enabled only
after the ready, acknowledged, non-modal/non-closing playable panel succeeds.
The existing request snapshots `armed && eligible`; queued loading stages also
fail the worker's live-actor gate. Only the showcase uses its actual source-crop
bounds, allowing existing larger-viewport centring at the unchanged fit scale.
Normal worlds, camera math, simulation, lighting and art inputs are unchanged.
Review caught one more UI detail before execution: `CloseCurrentPopup` does not
erase geometry already emitted by `BeginPopupModal` in that frame. The existing
progress draw now returns its unchanged action plus an actual `popup_began`
observation. The playable branch passes that local fact into eligibility; no
timer, frame count or persistent screenshot protocol was added.

These fixes passed independent production source review. The actual camera
snapshot centre CPU test passed in 0.114 seconds. The Game session test and real
CPU ImGui open→closing-with-vertices→absent test passed together in 0.164 seconds.
The updated ordinary Editor eligibility/readback regression passed (82.56-second
leaf, 89.31-second process). The earlier 116.792-second Core/GPUAV pass is reused
for the unchanged Renderer/RHI copy, fence and resource-lifetime implementation;
the subsequent changes are CPU capture policy, UI observation and camera bounds.
Build and test records are under `playable-readiness/popup-draw-fact/`, including
the preserved test-only popup-query lifetime correction. Correctly framed ready
4K A/B image results follow below.

### Ready 4K comparison and next visible phase

Both renderer-owned 3840×2160 captures are now visually confirmed ready: no
progress modal, native toolbar present and the authored island centred. They
use the same binary, world/content, exposure EV3, sources, assets and camera
revision 4. The corrected camera corner is approximately
(-1.7777767,-0.9999995,-0.5), at scale 0.0064814813. Their only staged shader
change is the cut-face constant 0 versus 0.04.

- [Backwall-only control](../../out/diagnostics/environment-lighting-20261102/v22-reassessment/initial-playable-window/presentation-experiment/runs/ready-backwall-only/final-frame.png)
- [Backwall plus 0.04 cut-face light](../../out/diagnostics/environment-lighting-20261102/v22-reassessment/initial-playable-window/presentation-experiment/runs/ready-canonical-04/final-frame.png)

The PPM-to-PNG conversions preserve every RGB byte. Each readback has 33,177,600
GPU/CPU bytes and pitch 15360; its own completed fence is 1193 for the control
and 1070 for the canonical run. Field admission is identical: 589,824 queries,
2,304 rows, 258,048 field bytes and no coarse dispatches. The logged peak ledger
1,074,051,884 bytes includes the caller's 1 GiB reserve; it is not measured VRAM.
No producer GPU execution time, readback-wait time or map duration was logged.
Shader compilation times and startup wall time cannot substitute for them.
The complete pair provenance is in
`presentation-experiment/runs/ready-pair.json`. A separate 120,000-pixel central
backwall sample (x=1400..1999,y=500..699) is byte-identical across the pair;
`ready-background-invariance.json` records this presentation-only check. It is
not a replacement for the physical error/leakage masks.

The canonical image reveals terrain banks while retaining the backwall's amber,
violet and cyan illumination and dark platform silhouettes. Independent Astra
visual review recommends **keeping 0.04 as the showcase baseline**. This is a
readability result, not a reference-art pass: machinery is absent, emitters look
like flat proxies, the rock/background treatment is simple, and the walking route
is hard to read. The unchanged 5.7851%/zero-leak control result still qualifies
only the bounded straight-ray solver.

Both runs reported no VUID/validation error but exited 1 with the same libdbus
allocation stack class, now 1,878 bytes in three allocations. Clean shutdown is
still unmet; no suppressions were changed. The repeated zero-group dispatch
warnings also remain unresolved (37,224 control and 36,864 canonical). Neither
the 4K captures nor their resource sizes establish 60 FPS or the lighting budget.

Coordinator accepts Astra's next bounded phase: **one playable industrial service
bay in this same scene**. Mount a lamp housing to the central ledge, connect a
pipe/elbow toward the cyan tank proxy, add a metal tread and supports at colonist
scale, and taper the adjacent sediment bank around a dark ore pocket. Reuse the
catalog/Grid material and normal shading for terrain and solid obstructions;
reuse compatible existing asset instances for decoration. Do not introduce a
new viewer, general prop framework, physics bypass or unlit decorative fallback.
Keep the cyan insert explicitly an opaque proxy until real liquid transport is
qualified.

Verify the bay in the actual game at the same exposure and corrected camera:
mounted lamp, connected pipe, readable traversable ledge, actor/collider agreement,
and a walk/pan/reveal using supported game controls or acceptance hooks. Measure
lighting and scene-draw GPU time on ordinary uncaptured frames, and actual field
update age. Follow this art phase with one working door through Driver-owned
edits and matching collider/material/Grid/light-field revisions. Demonstrate
blocked movement/light, then passage and illumination of the far wall. Fine
screen-space refinement follows that interactive test; exact-world precision
work and a full-world rewrite do not precede it.

### Industrial bay checkpoint and bounded correction

The first actual ready 3840×2160 industrial-bay frame is
[captured here](../../out/diagnostics/environment-lighting-20261102/v22-reassessment/industrial-bay-metrics/runs/ready-bay-canonical-04/final-frame.png).
The real Driver walk and tank-contact regression passed in 9.78 seconds.
The tread, supports, pipe elbow and tank form one connected installation, but
the amber pool is trapped above the central ledge and the walking route remains
too dark. This is visible progress, not reference-quality acceptance.

Coordinator accepts Astra's controlled correction: move the existing four
amber cells from x30–31/y21–22 to x30–31/y16–17 beneath the y18 ledge, open below
and to both sides. Preserve emitter size/strength, the other sources, seven
clear actor rows y7–13, exposure, camera and 0.04 cut-face response. Compare the
same receiving-wall and tread regions separately from the white bulb. Keep the
walk/contact check; only then add the working door across a visible warm pool.

The separate uncaptured measurement run produced one valid cold producer GPU
observation: **35.601728 ms**, submission 515, world 1/content
13357860154626364776/revision 1. It brackets the original producer encoder and
mandatory statistics copy, excluding CPU preparation and source uploads. This
is neither a warm distribution nor 4 ms qualification of scalable RC. The small
straight-256 control is still the interactive solver.

Whole-frame qualification failed: zero of 256 requested warm samples, with the
window marked interrupted. The reporter conflated an ineligible packet with an
identity change; ordinary physics-pending updates can return before its ready
assignment. One successful clean-ready frame join is proven by the recorded
surface generation, but the first interruption branch was not recorded. Fix
only this reporter policy and add bounded exclusion/interruption diagnostics
before another measurement. Do not weaken current-ready provenance checks or
report zero samples as a performance result. Original run evidence is retained
under `industrial-bay-metrics/runs/uncaptured-metrics-256/`.

Both bay runs had zero VUID/validation errors and the known libdbus allocation
stack class at shutdown (1,854 bytes, exit 1). Clean shutdown, physical scanout,
warm frame cadence and actual edit-to-display age remain unqualified. Native
solver ordinary/Core GPUAV checks passed without changing the physical masks
or thresholds. Future runtime results must identify the revised source/binary;
these observations belong to the original bay placement and reporter.

The under-ledge correction is now captured at
[3840×2160](../../out/diagnostics/environment-lighting-20261102/v22-reassessment/industrial-bay-metrics/runs/ready-bay-under-ledge-canonical-04/final-frame.png),
PNG SHA-256 `d59fc37c178ab425cad410ca94ccaeb6631c447764ec1294b5fd16d42c04056d`.
The refreshed real Driver walk/contact check passed in 10.08 seconds. Astra and
coordinator accept this placement: the backwall behind the colonist and tread
now receive amber light, while the tank remains cool. The same two 200×200
receiving-wall regions were black in the prior frame and now show amber; the
raw display-RGB comparison is retained in
`industrial-bay-metrics/runs/under-ledge-fixed-regions.json`. It is an image
comparison, not a replacement for the physical brightness/leakage gates.

The ready image contains no loading popup. Native surface readback completed
at fence 1257, pitch 15360; decoded PNG/PPM raster bytes match. This run had zero
VUID/validation errors, 34,344 zero-group warnings and the same three libdbus
allocation sites at shutdown (1,818 bytes this time, exit 1). Do not report clean
shutdown. The reporter fix passed source review and focused build checks.

The separate uncaptured run completed **256/256** warm matched whole-frame GPU
samples: average **9.723 ms**, p50 **9.618 ms**, p95 **11.359 ms**, maximum
**12.018 ms**. It observed one producer encode-plus-statistics-copy interval of
**3.92992 ms**, submission 1111; this is not a producer distribution, and the
changed lamp/mount and run conditions prevent attributing the difference from
35.6 ms to a specific optimization. Field storage remains 258,048 bytes for
2,304 rows; the 1,074,052,100-byte peak ledger includes the caller's 1 GiB
reserve and is not measured VRAM. The window stayed on surface generation 1,
with 270 clean-ready and 256 warm clean-ready results. Strict filters excluded
2,611 timing samples, with 1 missing submission provenance and 1,388 duplicates.
The complete record is
`industrial-bay-metrics/runs/uncaptured-metrics-256-after-surface-fix/terminal-status.json`.

The main-side eligible present-result receipt gap averaged **147.15 ms** (max
194.59 ms). Retained/not-ready presentations are excluded, so this is not
physical scanout or a measured display FPS. It also does not qualify normal-play
simulation/update cadence. The run had zero VUID/validation errors, 35,064
zero-group warnings and the same unsuppressed 1,818-byte libdbus shutdown issue.
4K60, complete lighting cost, dynamic update age and full-scope storage gates
remain open; no acceptance target changes.

Read-only cadence follow-up establishes that the 147 ms value measures only
main-thread receipt gaps for the ready/warm/provenance-filtered subset. This run
did not enable `--worldgen-report`, record Driver tick/actor-picture revisions,
or emit update/physics CPU timings. Existing Linux `hal::ProfileScope` is a
no-op, so source scopes alone establish no CPU measurements. The 2,732 skipped
not-ready packets and 3,852 frame-provenance misses (which can include bounded
ring expiry) do not prove CPU-bound simulation or slow physical presentation.
Reuse `--worldgen-report` for the next accepted runtime to observe existing
admission/CPU diagnostics, labeling its per-phase maxima separately from the
256-sample GPU window. Add a bounded window-aligned aggregate only if those
existing observations cannot answer the concrete question; do not create a
profiling framework or infer FPS from this filtered subset.

Next bounded gameplay experiment: a Driver-owned one-cell door near x36 across
the tread, seven actor rows clear when open and a fixed lintel to the ledge.
It must block passage and the amber light pool when closed, reject closing on
the actor, and publish cells/collider/lighting under one revision. Compare the
change in amber on a declared tank-side backwall patch; cyan/violet are valid
sources there and must not be mistaken for closed-door amber leakage. Square
lighting steps on the sloping bank and polygonal pipe-elbow patches remain
fine-visibility comparison crops. Neither art acceptance nor full performance
qualification is established.

Before any door capture, the visible comparison patch is fixed at native-output
pixels `[2250,1520,2400,1600]`, above the tread and on the tank side, with the
actor stopped on the same left side in both states. This supersedes the earlier
higher `[2250,1300,2400,1460]` patch: the newly specified fixed lintel blocks much
of the lamp path to that higher area even when the door is open. The declaration
and reason are retained in `industrial-bay-metrics/door-visible-comparison-plan.json`.
No physical aperture/leakage mask or threshold changes.

Coordinator accepts Astra's bounded door transaction design:

1. Use a stored mechanical door with existing material IDs and no dig particles.
   Initially leave its seven cells open to preserve the walking baseline; install
   the fixed lintel. Reject closing on the actual actor. Give each admitted
   change a Driver-owned monotonic revision that cancellation never reuses.
2. Freeze simulation advancement while preparing the existing edit candidate,
   inactive colliders, Grid uploads and immutable optical source from the same
   candidate cells. For this transaction, gate the attached-matter preparation's
   existing early settle. Keep native UI and the old complete picture available.
3. Carry the revision through prepare, activate, cancel and acknowledgement,
   including the lighting revision actually consumed by the rendered frame.
   Activate Grid plus field together; settle Driver cells/colliders/matter only
   after acknowledgement of that exact activation, while simulation is frozen.
   A held picture retains its old revision. Resume input/simulation only after
   the matching clean presentation and commit.
4. Before activation, cancellation retains the old state. Once activation is
   sent, an absent acknowledgement means unknown outcome: resolve the existing
   settlement path before cancellation or another change. After confirmed
   activation, an unexpected irreversible commit failure uses the existing
   terminal contract. Do not promise rollback across that boundary.
5. Reuse the existing panel intent and Driver/Editor transactions; add no queue,
   solver, prop framework or reload. Verify closed→open→closed, collision and
   passage, overlap rejection, stale/canceled revisions, and recoverable prepare
   failure retaining the old cohort. Record edit-to-matching-present latency and
   compare amber change on a fixed tank-side backwall patch.
6. If the existing one-shot capture cannot exercise the sequence, use one
   default-off, showcase-only acceptance/output option in the actual game,
   mutually exclusive with independent metrics/one-shot capture. Drive the same
   panel intent and actor-input/Driver paths, wait for actually consumed matching
   revisions and clean presentations, and reuse the existing Renderer readback.
   Capture the comparison states with the actor stopped on the same left side
   before crossing; then verify crossing, overlap rejection and final closure.
   Reuse one Application/device/pipeline setup across the sequence. Keep a bounded
   deadline and explicit failure result. This is programmed intent validation,
   not native-input or physical-scanout latency evidence, and adds no exported
   test framework or separate viewer.

Sequential captures require an explicit success-only Editor rearm: keep ordinary
`requestFrameCapture` lifetime-one-shot semantics, but carry a monotonic capture
request sequence through existing frame request/result transport. Main and
worker sequence state remain separately owned. Only the matching successful
owned-pixel result can permit the next request; stale results cannot clear a
new request, and failed/unknown submissions do not permit rearm. The worker keeps
the existing proven pre-acquire transient behavior. Reuse the Renderer staging
slot, which already clears after the real completion fence and successful unmap;
do not reset a worker-owned one-shot flag from the main thread or change RHI
submission/fate semantics. Verify two captures in the same existing Editor GPU
fixture rather than paying pipeline creation per state.

Revision clarification accepted by Astra/coordinator: the never-reused Driver
door/optical transaction identity is distinct from Matter's existing strict
source+1 publication/terrain counters, whose target can repeat after a canceled
attempt. Bind the outer identity to the exact candidate chunk manifest and
Matter source/target; do not change Matter's counter contract. Before reusing a
canceled internal target, confirm cancellation and settle every outstanding old
operation. Retain the binding until settlement; an unknown outcome prevents
another attempt. A late outer acknowledgement for canceled A must not affect B,
even when B uses the same internal Matter target. “One published revision” means
one proven coherent door-change cohort, not numeric equality of unrelated
internal counters.

### Service-door source checkpoint: execution pending

The coherent implementation is source-frozen in
`service-door/source/freeze.json`, SHA-256
`208f74f52fcba3b220021fefd808469425b9b654965ccc14d92ef7b8ecd9e10c`.
Its 31 changed paths include 25 C++ files, exact pre-door worktree copies and
door-only deltas. Scoped format/Check/semantic checks passed; the postformat
manifest is `service-door/source/format-review/formatted-manifest.json`, SHA-256
`30d1c7163f9e9ef1f6af7205f4bb4d27573396699f80a997782b396e9fc98639`.
This is unbuilt/unrun source, not proof of the playable sequence.

Independent review found the normal activation→Driver commit→matching-present
ordering coherent, but three failure paths block execution: an unknown field
submit could appear idle and permit cancellation readiness; a definitively
unsubmitted callback that failed partway through encoding needed explicit
`discardUnsubmitted`; and a teardown pump/transport failure could leave the
caller waiting forever for a lost activation acknowledgement. Preserve unknown
fates until explicit quiescence, discard only known unsubmitted work, and return
unknown plus the retained error when teardown cannot settle the acknowledgement.
The author also identified 43 required paragraph separators after formatting.
One aggregate fix is in progress; builds/tests remain held until that fix is
frozen, formatted and reviewed.

The aggregate correction is now frozen in
`service-door/source/aggregate-review-fix/freeze.json`, SHA-256
`4eeb6fdd0fc053a6870083c865ef5afc834736a4419a972d5951589c17824d4e`.
Postformat manifest `aggregate-review-fix/postformat-manifest.json`, SHA-256
`243c136987ff49859a29a90c991459563bada3960b6a8bf56e4abe1fe0cfffc8`,
records all 31 current paths. Scoped format/Check/semantic checks, the final
author paragraph audit and targeted independent source review passed. The
existing Editor fixture now uses the existing Renderer submit-failure seam to
check that unknown submission blocks cancellation/reuse even after later idle.
Partial compute-pass failure and teardown pump/stash loss lack suitable existing
injection seams and remain source-inspected, not runtime-injected. No mock
framework, producer-math, shader, Renderer or RHI change was added for these fixes.
The game target built successfully in 34.7 seconds; remaining affected builds,
focused tests and the actual-game sequence are still pending.

All four affected targets are now built: `app.game`, `game.tests`,
`engine.tests.app` and `engine.tests.asset`. The first asset-test build exposed a
single Mango vector assignment requiring `int2{64,64}`; its one-site fixture fix
and the failed build are preserved under `aggregate-review-fix/compile-int2-fix/`.
Only the asset target was rebuilt (14.1 seconds), and the three unaffected build
results were reused. Final 31-path manifest
`compile-int2-fix/postcompilefix-manifest.json` has SHA-256
`b115a4c171594757aa423117d50d4b0e3ce607a169306becc292c59f772160d0`.
Sanitizer/configuration policy is unchanged. Focused tests and the actual-game
door sequence remain unrun at this checkpoint.

Focused CPU config, real Driver route/door and session-presentation selectors
passed together in 10.86 seconds. The new attached-Matter cancellation/reused-target
GPU leaf executed once and passed in 12.03 seconds with Core and GPU-assisted
validation. The ordinary asset invocation passed Retro lifecycle (130.34 s) and
HDR rejection (11.40 s), then failed the Editor fixture at candidate-A admission.
The full replacement fixture uploaded a 128² tile but omitted its required
`m_dirty_mask=1`; the production optical guard correctly rejected it. The
fixture-only correction is frozen in `service-door/source/fixture-dirty-mask/`.
Reuse the two passing ordinary leaves and rerun only the affected Editor leaf.
The failed invocation and diagnostics remain under
`service-door/runs/asset-focused-ordinary/`; it was not a timeout.

Before the actual-game run, a Game-only witness correction in
`service-door/source/closed-walk-witness/` adds an actual closed-door walk/contact
check, not just the already passed CPU collision test. Hold right for at least
12 Driver ticks at the door, stop on a later zero-command tick, and capture both
states at the same stopped left-contact pose before crossing. Input ineligibility
cannot count as successful blocking. Both small corrections require scoped
formatting and only the affected game/asset builds; other binary/test closures
are unchanged. The integrated game, paired images and GPU-assisted Editor pass
remain unverified.

The authored diagnostic option is
`--lighting-showcase --lighting-showcase-door-check=<existing-directory>`.
It writes `service-door-closed.ppm` and `service-door-open.ppm`, then verifies
crossing, overlap rejection and final closure in the same Application. Success
requests normal shutdown; the internal 180-second ceiling begins at the first
Game update and includes loading. Use a 240-second external bound with 10-second
stop grace and no old 60-second generic exit timer. These operational bounds
are not performance acceptance criteria. The first actual check timed out as
recorded below; paired door images remain missing.

### Actual door run and scheduling diagnosis, 3 November

The first actual-game door check is terminal: unit
`ppr-service-door-check-20261102.service`, MainPID 0, exit 1. It reached the
3840×2160 surface, field admission and world publication at 21.021 s, then hit
its internal 180-second check deadline at app-relative 185.199 s. It produced
neither PPM nor a completion marker. The external 240-second cap did not fire.
Evidence is under `service-door/runs/actual-game-door-check/`, with
`diagnosis-summary.json` SHA-256
`5678f75853618b5e0f567f889fdec0dc370a93e44484ecaf37429163fcb73ce8`.

The report contains 10,712 live updates and 10,711 generic presentation receipts,
but no door phase/revision/readiness record; those counts do not establish door
progress or physical FPS. Only the initial Matter GPU tick was recorded. The
run had no VUID/Validation Error matches, but shutdown also reported cancellation,
scheduler join timeouts and the recurring libdbus leak class (1,866 bytes).
No focus manipulation, process attach or automatic retry was performed.

Read-only source analysis found a reachable scheduling deadlock: Game's generic
pending-Matter/edit shortcut calls `Driver::update({})` and returns before the
door coordinator. Once `prepareServiceDoorSnapshot` owns quiescence and starts
an async snapshot, Driver update intentionally returns without polling it.
The same shortcut can starve the subsequent prepared edit. Exclude active door
transactions from that generic shortcut so the existing session/ACK path reaches
`advanceWorldJob_`/`advanceServiceDoor_`; retain the physics freeze and backlog
rules. This matches the failure pattern, but the old log lacks phase data to
prove that it caused this particular run. Add bounded phase/milestone snapshots
and full timeout facts before the next run, without extending its deadline.

The isolated Editor retry failed earlier, at its immediate post-resize padding
precondition, so it did not reach the corrected dirty-mask case. The window API
issues a native resize and immediately queries geometry; it promises no settled
resize. The proposed bounded fixture repair uses existing event waits/queries
and preserves even/non-64-width and positive actual readback padding checks.
Its early failure also exposed expected lazy-surface absence at Editor teardown;
query that absence on the render owner rather than hiding a real destruction
error. These independent repairs remain held while the Game scheduling fix is
prepared. All unresolved acceptance gates remain open.

The Game-only scheduling correction passed targeted independent review and
build; its new run directly reached door revision 2, prepared Grid/field,
activation, Driver commit and a clean matching presentation. The admitted
close→present receipt measured **3,074.879881 ms**, exceeding the unchanged
one-second target in this observation; it is not qualified by the earlier
3.93 ms producer GPU measurement. The actor reached the closed door and held
contact for 15 actual ticks (111→126). At 27.784 s the diagnostic then failed
its exact-zero horizontal-velocity check before any PPM was written. The final
status was exit 3/SIGABRT after a separate Slang-RHI live-device assertion.
Evidence is in `actual-game-door-check-scheduling-fix/terminal-summary.json`,
SHA-256 `56e5606fa030c20f4f531aab8aab0e569446200d68d1fcc1513f66fa047375f4`.

The stop boundary at tick 126 explicitly had a physics slice in flight. Clearing
PlayerCommand does not change that already admitted right-command slice, so
merely observing a later completed tick does not prove a zero-command step.
Wait for at least two completed ticks after clearing under the existing serial
one-pending-slice contract, then observe actual velocities and stable contact.
No floating-noise magnitude or velocity tolerance is inferred from the failed
run: its failing velocity and tick were not logged. Keep the strict velocity
check until a proven zero-command observation is recorded, and add bounded
pose/velocity diagnostics.

Independent ownership analysis also found unconsumed Editor main staging can
retain real particle-view buffer references when update fails and render is
skipped. The run recorded positive quiescence and pass shutdown before the
live-device assertion. Release those staging owners after proof/drain and before
passes/RHI, without weakening unknown-fate retention. This is a source-proven
defect, not a measured inventory of every live object. Extend the same Editor
application fixture with a sole real view staged but never rendered; no new
device/pipeline setup is needed. The bounded geometry settlement and expected
absent-surface cleanup can be verified in that same batch. All paired-image,
warm update-age, GPU-assisted Editor and full-product gates remain open.

The three-file stop/ownership/geometry correction passed targeted source review
and author grouping/layout review. The asset fixture needed an explicit X/Y
comparison because Mango's integer-vector equality overload does not compile;
that fixture-only correction preserved both native/cache dimensions. All four
affected targets then built successfully. The ordinary shared Editor regression
passed **1/1 in 110.69 seconds**, including the existing same-application capture,
candidate/cancellation/unknown-fate cases and the new unconsumed real-view teardown
and absent-surface path. Evidence is under
`service-door/runs/editor-ordinary-stop-ownership/`, log SHA-256
`f4990fc7deed4502a5e4096c8011ecc778f93dc450ca8781f8e7b41672742d4e`.
The next actual-game run is launched with the same 240 s/10 s external bounds;
its result and paired images remain pending. GPU-assisted Editor validation is
still required after that run. Passing this fixture does not establish the
game's stopped-actor witness, a unique cause for the previous live-device count,
or the one-second update-age budget.

The subsequent `actual-game-door-check-ownership-fix` run reached matching closed
publication, but native focus disabled its scripted walking command. It failed
the explicit command-owner precondition before either capture. No live-device
assertion or SIGABRT recurred in this failed-update path. The observed first
close→present receipt was **29,520.678236 ms**, again exceeding the unchanged
one-second target. This does not identify whether CPU preparation, GPU work or
scheduling dominated. The known libdbus LeakSanitizer report remains unresolved;
no suppression was added. Terminal summary SHA-256:
`258f4459d9a5a2580b2c72def026e21e386f79664928d4a081e4c17d5ad44661`.

Coordinator decision: let this already scripted, default-off checker own its
programmed PlayerCommands explicitly, while native keyboard/mouse retain their
focus rules. Do not focus the window or declare native-input qualification.
Record successful command issuance, actor, actual lighting revision and an
uninterrupted run identity. Both the two-tick zero-command wait and twelve-tick
stability witness must belong to that same run; interruption after the window
starts must fail rather than combine old and new evidence. This last continuity
correction passed independent source review, post-format author review, scoped
format/style checks and the `app.game` build (19.36 seconds). Formatted Game
SHA-256 is `b79253f44d9a8364df19255c3b8285cf8779ce6517e37eec05eeda8a4b855a1a`;
binary SHA-256 is
`1abc46210790a2fc1545300d617bbdb7d8b4d9761c2efadb66be9d634133f7c3`.
The next single actual-game run is authorized with the existing time bounds;
source review alone does not prove the stopped-contact/capture outcome.

That run reached closed revision 2's clean presentation at 23.286 s, with
**2,910.116992 ms** close→present age. It held rightward contact for 15 ticks
(113→128), issued zero at tick 128, then failed exact post-step horizontal
velocity at tick 133: vx = −0.040824633 m/s, vy = −0.040824752 m/s. No final
images were written. Corrected terminal summary SHA-256:
`94285be0f950ac5c435b54ac7c46cc143d2cfd6c722c0d9ff42283d0e9c5b7b9`.
Later `door=false`/revision 0 milestone fields describe an absent transaction;
they do not contradict the explicit closed-revision presentation event.

Source diagnosis establishes that zero PlayerCommand sets desired horizontal
velocity **before** Box2D integration; post-contact velocity is mirrored without
a zero guarantee. The door/tread junction has a diagonal collider consistent
with the observed sliding. This is not a measured contact-manifold attribution.
Opening at that corner also changes support, making it a poor anchor for fixed
comparison pictures.

Astra/coordinator decision: retain the actual closed-door right-command contact
and nonpenetration proof for at least twelve committed ticks, then walk left
through the same real command path to native-cell centre x ≤ 34 on the unchanged
flat tread. Check the actual footprint clears the chamfer and remains supported.
Issue zero, retain the uninterrupted-run/T+2 proof and require twelve further
committed ticks inside the existing ±0.05 m position envelope with finite pose
and velocity. Remove exact post-solver velocity equality; introduce no velocity
epsilon. Pin this flat pose for both images. Opening gets its new revision's
own zero-command prefix and twelve-tick stability witness at that anchor. Keep
crossing, overlap rejection and final close afterwards. This changes a checker
assumption, not physics, geometry, the physical 15%/1% gates or any performance
target. Actual paired captures and full-product acceptance remain open.

### Matched playable door result and next experiment

The flat-tread checker completed at 37.482 s with two actual 3840×2160 captures.
It held right-command closed-door contact for 15 ticks (115→130), walked left
to the flat tread, certified zero-command stability for 15 ticks (145→160), then
certified the open revision at that same anchor for 13 ticks (168→181). It crossed
the open door, rejected closure on the actor and presented final closed revision
4. This validates programmed Driver commands, not native keyboard latency.

The pair is under
`service-door/runs/actual-game-door-check-stopped-flat-witness-20261103/door-output/`:
`service-door-closed.png` and `service-door-open.png`. Their decoded RGB bytes
match their PPM readbacks exactly; root visually inspected both. They share
camera/content/job/world and actor anchor, with optical revisions 2 and 3.
The predeclared backwall ROI `[2250,1520,2400,1600]` is black when closed and
mean RGB `(75.004,44.029,0)` when open. This is a visible receiving-wall shadow
result, not physical leakage certification from tone-mapped RGB. The scene still
looks flat and sparse relative to the references; this is not art acceptance.

Close/open/final-close present-result receipts measured respectively
**3,021.887853 / 2,932.422047 / 2,942.665060 ms**, all above the unchanged one-second
target. The completed check was followed by exit 1 from the known libdbus LSan
class (1,542 bytes in three allocations); zero VUID/validation errors were
reported. Terminal summary SHA-256:
`67202577cce44f96ad530bafab8658e5f3c12da98c55028b0b3d10b49729e6e6`.
Game source `dc58a1f75c4e76d7047eebc679c0a6376804ce9fb053763157678912efaa4ea2`
and binary `17e57b91c0f9de274a447ea0774b7c5a1722487b4cafc778844bf1d1bf20f247`
identify this run. Reuse its functional result; investigate age with a separately
labelled Nsight capture, preserving sanitizers and the frozen runtime assets.

Astra/coordinator authorize a default-off **backwall-only** fine-visibility pilot.
Reuse Grid's actual displaced contour selector to rasterize one centre-sampled
R32Uint terrain-material mask; depth alone cannot classify 2D holes. Keep solid
foreground unchanged until its exposed fine-boundary origin is defined. Evaluate
the existing sixteen directional samples once per cached background pixel in a
fixed door/jamb world region, capped at 128×256 evaluated raster pixels, with a
two-cell mask-read halo. Use a two-cell near interval and eight fixed taps as a
sampled approximation, not certified visibility. First fine opaque hit returns
its true source once; otherwise query signed directional world CH at the near
endpoint or actual screen exit, retaining offscreen transport. Preserve signed
values through the angular sum. Do not uniformly darken a preconvolved mean.

Estimated bounds are 4.19 million mask fetches and 524,288 directional endpoint
evaluations per view, plus raster cost. A full 1080p R32Uint mask is 7.91 MiB,
23.73 MiB for three slots before overhead/overlap; these are not measurements.
Both background paths consume the same centre-density result. No screen history
is introduced: current camera, extent, layout and actual geometry/light cohort
must match; held completed pictures retain their existing lifecycle.

The pilot's declared native-cell region is x `[34,38]`, y `[7,15]`. Evaluate at
actual current reduced-resolution raster centres in the existing background
cache, not a new world-fixed 32-samples-per-cell reconstruction grid. Project
that world region into the current viewport and cap its evaluated rectangle at
128×256 raster pixels around the projected centre; record the actual rectangle,
active count and clipping. Outside that explicit diagnostic region remains the
world-only control. This bounds work during pan/zoom without changing physical
quality masks or claiming the rest of the image is refined.

For this default-off pilot, host admission validates the geometry/palette,
camera/cohort and existing finite presentation bound. Per-query GPU status is
retained for focused readback checks; invalid rows render explicit magenta and
fail visual acceptance, rather than silently using black or the world control.
No per-frame synchronous validation wait or generic asynchronous framework is
introduced. This diagnostic policy is not wider production qualification.

Compare closed/open fine edges with a denser raster-visibility reference and
include unchanged-silhouette and clear-region controls: endpoint resampling of
truncated CH can alter bands even without a geometry correction. Measure actual
active pixels, steps and GPU mask/reconstruction timestamps separately from
capture overhead. Stop before wider rollout or MSAA planes if this bounded pilot
does not improve local quality at a practical cost. No CPU cascade clone, new
viewer, full-world rewrite or quality-gate exemption is authorized by this pilot.

The first useful Nsight capture completed on the frozen baseline binary. It
contains individual Vulkan workloads and API events, not just a nonempty report.
All 57 Vulkan pipeline-create calls totalled **9.335 ms** across that run; those
API calls do not explain the approximately 2.8-second edit receipts. This does
not measure shader preparation outside Vulkan creation. In inferred edit windows,
queue-idle host calls totalled approximately one second each; open/final-close
each overlapped an approximately 1.3-second queue-work interval. Pass attribution
is unresolved. Rounded application logs were aligned to the trace using four
matching creation events, so the edit-window bounds are approximate.

CPU sampling was configured, but the exported data did not yield resolved CPU
hot stacks; available OSRT callchains contain raw addresses. Do not infer a
CPU-bound shader compiler or a particular lighting pass from this evidence.
The profiled process completed the door sequence but LSan reported 1,738 bytes
in six allocations, combining the known libdbus class with added profiler/X11
and pthread classes. No new suppressions were added. This externally contended,
instrumented run establishes neither 4K cadence nor a GPU lighting budget.
Analysis: `service-door/runs/nsys-stopped-flat-door-check-20261103/nsys/profile-analysis.md`,
SHA-256 `3b1df30f5c6dacb606b81dc70f9d48462acf5c6e9f694eaf11576234440c9d2d`.
Reuse the existing per-generation WorldRadiance GPU measurements for the next
explicit door/pilot run to distinguish producer execution from update age;
their documented scope excludes CPU preparation and source uploads.

The first pilot freeze contains fifteen paths (eight C++/module files) and
extends one existing shared native GPU leaf. Game and game-test targets built;
the renderer-test target exposed local fixture type/import and initializer
errors. Coherent review also requires selected-background-slot generation
ownership for compute/mask roots, correct fixture camera bounds, inverse-matrix
admission and invalidation of prepared rows on world activation. These are one
bounded correction batch. The three targets subsequently built and targeted
review closed all four findings. Preflight corrected the fixture's Core/GPUAV
coupling, then the first ordinary run exposed vacuum ID 0 incorrectly included
in its Grid visual inventory; excluding that row preserved native vacuum and
all production admission rules.

The corrected shared leaf passed ordinary in **12.82 seconds** and GPUAV in
**16.95 seconds** (17.84 s CTest wall). The GPUAV log confirms both validation
modes enabled, with zero VUID/validation-error hits. Original physical gates
remain **5.78512% brightness error, zero blocked-side leakage and zero matched
straight-ray traversal discrepancy**. New mask, row-status, dense-point,
1×/4× consumer, camera, cohort and fence assertions passed in the same fixture.
The new opt-in configuration leaf passed separately. These are functional
results; fine-edge visual acceptance and isolated GPU cost remain open.
Separate mask/refinement GPU timestamps remain unimplemented and unmeasured.
Two actual 4K door-check runs on the same current game binary/assets are next:
world-only and pilot, changing only the explicit pilot flag. Preserve the
predeclared world region and holdouts when grading the images.

The same-current-binary 4K pair completed both door checks and passed Core
validation. Root and Astra inspected the original frames: the pilot's open-door
edge adds a stair-stepped amber wedge ending at the diagnostic rectangle, while
the world-only frame reads more clearly. **Keep the pilot off; no wider rollout.**
The region is raster `[1011,617,1115,823]` (104×206, 21,424 centres), corresponding
to capture `[2022,1234,2230,1646]` under the exact 2× spatial route. The unchanged
wall ROI `[2250,1520,2400,1600]` lies outside it and is a holdout, not evidence
qualifying the corrected edge. There were no magenta pixels in the evaluated
capture patch. Pair evidence:
`screen-contact-pilot/runs/door-check-matched-pair-20261103/pair-analysis.json`,
SHA-256 `7fb37b859c9039a613eff61106072e881d30edad16d44157f31eed5b62a59083`.

Producer encode timestamps for initial/closed/open/final-closed generations were
35.728/4.506/3.486/3.485 ms world-only and 35.763/3.485/3.485/3.484 ms with the
pilot. The corresponding edit receipts remain approximately 3.6–3.7 seconds.
This separates producer execution from the large update age; it does not measure
source preparation/uploads or the pilot's mask/refinement, and does not qualify
the four-millisecond or one-second gates. Both processes still ended with the
known libdbus LSan failure after completed checks. If fine visibility is revisited,
compare one dense visibility control with identical endpoint radiance rather
than widening the region or starting a parameter search.

The next art comparison uses the isolated, independently copied worldgen roots
under `material-lookdev/sandstone-coarse-authoring/engine-ab-source-roots/`.
Exactly three existing files differ: sandstone albedo, normal and appearance
descriptor. Pack entries, IDs and transport/emission fields are unchanged;
the pack manifest remains 65,468 bytes with SHA-256 `9825e049cd09a55b79f299f47481a75f046c451daac5ddc261fe1c90ea212012`.
Run the current game with the candidate root and screen pilot off before any
source-asset promotion. Real import, visible grain scale and tangent orientation
remain unqualified by the offline proofs alone.

The isolated stone candidate completed the actual world-only door check and
wrote both 4K frames, preserving blocking/crossing and fifteen stable zero-input
ticks in each capture state. Root inspected both originals: pale stone separates
more clearly from clay and takes the coloured rim lighting, but grain relief
still reads weakly at this scale. Do not call it final art or promote source
assets yet. Run summary:
`screen-contact-pilot/runs/door-check-sandstone-candidate-20261103/run-summary.json`,
SHA-256 `dc99a9a9f8064d35766797cde3efe07e3ad14f990662c21316243e69cdbc6663`.
No Vulkan validation errors were reported; the existing libdbus LSan exit remains.

Source diagnosis confirms the mapped normal already modulates the independent
`.04` directional cut-face key, even when deep-solid world incident light is
zero. Do not "fix" an albedo-only key that does not exist. The current 0.25 m
period implies substantial normal mip filtering at this camera (estimated
LOD 4.73, not a measured sampled LOD). The next bounded observability change
reuses once-per-world prepared texture metadata/samples and existing diagnostic
flags, plus missing Grid/field-prepare phase boundaries, without a new public
API, clock service, shader probe framework or transport change. This will help
separate material import/filtering from shading and source upload/queue latency
from producer execution before another art or synchronization change.

### Colour-only material diagnostic

The separate colour-only stone trial is **not accepted**. One decoder-only
Blender read, with no rebake or threshold changes, found exact v2 equality for
height, grain mask, roughness and metallic; normals differ by one 16-bit code
step, violating the frozen exact-equality requirement. Its colour equation has
near-zero mean signed error, but boundary-localized maxima of 0.000754/0.000600
exceed the unchanged 0.00025 maximum. Independently filtered mask and cell tint
do not establish the filtered product used by the colour graph; the diagnostic
supports that attribution without qualifying a replacement export contract.
Keep this failed trial and the runtime assets unchanged. Report:
`material-lookdev/sandstone-colour-authoring/execution/decoder-only-20261103/decoder-report.json`,
SHA-256 `8f2a63d62e2430e01b64a6bab7f581f39986b7a64fdaf624b3d38ad62204fef1`.

The fresh `sandstone-colour-linear-authoring/` trial reuses the pinned v2
geometry maps verbatim and exports only colour/cell tint from the editable graph.
Its linear pigment `p=0.5+0.5*(cell_tint-0.5)` avoids the filtered-product mismatch.
The seed17/17/29 validator passed exact file/decoded geometry identity, repeated
seed identity, variation, metadata, normal/seam checks and the unchanged colour
limits. Verification SHA-256:
`7b15032e15c0cd44d7300159190707186d9a97f3a658cf483446501043ad3a0e`.
The six exported-map relit proofs were inspected. The transient Blender unit was
unloaded before its final `ExecMainStatus` query; retained outputs/verification and
journal establish completed work, but the raw child exit receipt is missing.
Do not rerun a bake merely to manufacture that receipt or report it as retained.

One independent 682-file scale-2 runtime root differs from its baseline only in
`textures/sandstone.png`. Its actual 4K game run completed the same door witness
and produced losslessly verified closed/open captures. The fixed background
shadow holdout is byte-identical; the predeclared ceiling-stone holdout has about
3.18 RGB code values mean absolute difference. The coordinator inspected both
frames: individual stone pigment is clearer, but the faceted cell patches still
fall short of rounded-pebble reference artwork. **Keep this as a comparison asset;
do not promote it to the default pack on numerical QA alone.** A future masked
pigment may require explicitly exporting the joint pigment term for correct
filtered verification, rather than letting the simple algebra dictate the art.
No new authoring run is authorized by that observation. See the trial's
`VISUAL_REVIEW.md` and
`engine-ab-source-roots/runs/v4-seed17-world-only/final-run-summary.json` under
the material diagnostic root. The actual game again exits 1 on the existing
1566-byte libdbus LeakSanitizer report; no suppression changed.

### Measured candidate preparation bottleneck and bounded remedy

The new phase records localize 2.326/2.419/2.330 seconds of the three edits to
Grid candidate preparation. Sequence spans of 137 mean **138 inclusive advance
calls**, matching the actual 138-image inventory (134 material images plus four
background layers). Field CPU preparation takes only 2–3 ms. Actual prepared
stone colour/normal images are 512² RGBA8 sRGB/linear respectively, with ten
materialized mip levels; sampled GPU orientation remains unqualified. The
single 0.5 m scale control produced only a modest full-frame change, so do not
keep rebaking or promote it as completed art.

Phase evidence is in
`screen-contact-pilot/runs/door-check-sandstone-scale2-observability-20261103/phase-analysis.json`,
SHA-256 `cfbbb2996a1964f078ae3131328587b132372a4f2fb131241ecd8e9f5cebda55`.
Source confirms one image-record budget per advance and a fresh private cache
per candidate, with no active-inventory reuse. Do not equate the measured span
with proven GPU transfer duration. Approximately 1.28 seconds before Grid staging
remains separate Driver snapshot/collider/Matter work. Retirement records were
outside those spans; no retirement-scheduling change is justified by this trace.

Astra/coordinator decision: share only the **sealed ordered terrain texture
inventory** between active and candidate worlds on an exact same-device and
validated immutable backing/layout/format/interpretation match. The inventory
owns its completed existing cache, descriptor buffer, handles, pinned images
and slot mapping. Each world retains an independent palette, atlas, geometry,
roots, lighting field and publication revision. A matching candidate takes a
strong inventory lease and skips the 138 image advances; a miss retains the
existing bounded one-image upload path. Do not copy naked cache-local handles
or introduce a pass-lifetime mutable cache: bindless slots are not recycled.

Nonfinal inventory release is constant ownership release; final inventory
destruction follows the existing paced handle/image drain after all required
reader/root/unknown-submit proofs. Preserve cancellation, old completed pictures,
source+1 Matter counters and the resource overlap bounds. Verify zero fresh
texture allocations/uploads for a warm matching inventory, changed palette
binding with reuse, active-reader survival/cancellation, and exact-identity
misses in the existing shared Grid fixture. Record reuse comparisons, actual
uploads/bytes and candidate advances separately, then repeat the real door check.
Do not raise an upload quota or weaken the four-millisecond budget to hide the
delay. The remaining pre-stage interval still prevents claiming the one-second
target achieved even if this measured candidate bottleneck is removed.

### Sealed inventory reuse: verified bounded result

The four-file implementation received independent Astra source review. Review
caught and corrected a mistaken allocation-flag requirement: production
`UniqueBuffer::moveToShared` images retain a mutable-origin allocation flag while
the published shared buffer provides const access and pins its backing owner.
Matching now uses validated owned/materialized backing identity, exact byte
ranges and ordered format/mip/layout metadata. No core flags, decoder behavior,
fixture image semantics, upload quotas or retirement quotas changed. Per-world
palettes and publication/reader leases remain independent.

The final source manifest is
`service-door/source/terrain-inventory-reuse/final-source/source-manifest.json`
under the diagnostic root, SHA-256
`57924f29a9daa614ab361d465c8271ba55c1df6729d4a71b3b5be0624079bbe0`.
Format/style and serial `app.game`, `engine.tests.app`, `game.tests` builds passed.
The existing shared native fixture passed ordinary and GPUAV in **12.49/13.61 s**,
including setup, with no reported VUID/validation errors. It reports 138 cold
advances versus one warm advance, zero warm new uploads/bytes, independent palette
pixels, pending-reader cancellation survival, identity misses and invalid-pitch
rejection. The unchanged physical result is **5.7851% L1 and zero blocked-side
leakage**. These are fixture results, not full-world resource qualification.

One actual world-only, scale-2 stone door run used app binary SHA-256
`01171bd09ea257bf88bb33f50991fe7010b14b6e808dfa2cc78f81d48449c990`
and the unchanged frozen isolated asset root. It completed blocking, stable
closed/open captures, traversal, occupied-door rejection and final closure. Both
3840×2160 PPM-to-PNG conversions preserved the raw RGB bytes. The coordinator
inspected both images; the receiving backwall changes with the door and its
fixed holdout `[2250,1520,2400,1600]` is byte-identical to the earlier matching
scale-2 run in both states. This preserves that visible behavior; it is not a
new physical leakage measurement or representative-art acceptance.

| Warm revision | Earlier Grid phase | Reuse Grid phase | Earlier receipt age | Reuse receipt age | Producer GPU work |
|---|---:|---:|---:|---:|---:|
| 2 | 2326 ms | 32 ms | 3631.023 ms | 1257.871 ms | 4.378 ms |
| 3 | 2419 ms | 28 ms | 3743.323 ms | 1267.203 ms | 7.545 ms |
| 4 | 2330 ms | 26 ms | 3629.350 ms | 1249.002 ms | 3.483 ms |

The run logged three reuse hits of 138 images each, with zero new texture uploads
and bytes. Cold initial upload totals were not logged in the actual game and
remain unknown there. This is one run against one earlier run, not a repeated
performance distribution. About **1.2 s remains before Grid staging**, derived
from receipt age minus the logger's stage-to-receipt span; the logger has 1 ms
resolution and does not separately timestamp source snapshot or native intent.
Receipt is not scanout. CPU field preparation remained about 3 ms. Producer GPU
timestamps exclude other lighting/rendering work and include samples above 4 ms:
neither the one-second age nor four-millisecond total-lighting gate passes.

Artifacts are
`service-door/source/terrain-inventory-reuse/runs/scale2-observability/phase-analysis.{md,json}`
and `fixed-wall-roi-comparison.json` under the diagnostic root. The process reached
its gameplay/capture success path, then exited 1 on the unchanged 1566-byte
libdbus LeakSanitizer class. No suppression changed; clean shutdown remains open.
The existing asset Grid empty-catalog/device-loss and repeated texture-catalog
lifecycle leaves also passed in one shared GPU application per policy: ordinary
process exit 0 in 145.46 s, then Core/GPUAV exit 0 with leaf durations
123.88/45.50 s. No VUID/validation errors were reported. These two focused leaves
verify the affected owner; the full asset matrix was not rerun. Logs and terminal
records are in `service-door/source/terrain-inventory-reuse/asset-lifecycle-runs/`.
This closes the bounded inventory ownership batch, not the full lighting goal.

The shared GPU-assisted render batch completed successfully in **330.06 seconds**:
Retro 179.52 s, HDR 16.57 s and Editor 127.04 s, with zero reported VUID/validation
errors. These three leaves reuse setup within their existing application fixtures;
they do not share one application across leaves. The 450-second external bound
held; advisory stall notices did not trigger a restart. Terminal artifact:
`service-door/runs/editor-scene-flow-gpu-validation-20261103/terminal.json`, SHA-256
`06e629f3770c3dc505cc1d41cb6cc23147c2ba40792976033d8dea1e5dd7468c`.
Game-only command-witness edits do not invalidate this unchanged render closure.
This functional run, with desktop applications active, provides no performance
or VRAM qualification. CLion MCP and a local Nsight Systems installation are now
available for readiness checks and bounded cost investigation; useful debugger
control and captured profiling evidence still need verification.

### Full-goal acceptance remains open

**Historical checkpoint; reconciled 2026-11-06:** the table below records its
original acceptance wording. The active decisions at the top supersede its4ms,
mandatory-cascade and appearance requirements: use6ms normal/10ms stress,
approved direct-native demand-window transport, and deferred visual matching.
Temporal2x/bloom, committed Matter projection and diagnostic gameplay queries
have later implementation/component evidence, but integrated qualification is
still open. The current requirement/evidence audit is
`out/diagnostics/windowed-native-20261106/remaining-acceptance-audit.md`.
Emitter relocation, full feedback/media/gameplay scope, cold-demand/edit age,
integrated cost/cadence, live-memory and clean teardown remain incomplete.

The small-scene control and capture work do not redefine the delivery objective.

| Required end state | Evidence still required |
|---|---|
| Representative playable environment | Door captures and lit receiving backwall exist; reference-level terrain relief, environment dressing and broader playable interaction remain unqualified |
| Meaningful doors and emitter movement | Bounded Driver-owned door witness passed; emitter movement and broader supported matter/edit cases remain unimplemented or unqualified |
| Scalable 4096×4096 world RC, including matter | Qualified cascade reconstruction and material/matter projection at full scope; the 4096-cell experiment cap and straight-256 mode are not this result |
| Direct/sky, diffuse feedback, participating media and gameplay illumination | Explicit source inventory, previous-generation feedback, qualified media transport and renderer-independent gameplay queries |
| Directional relief and rough specular response | Native CH rough specular and pipe correction passed the static small-scene checks; broader material response, fine visibility and motion/disocclusion remain unqualified |
| Selected Hable temporal reconstruction, exposure, AgX, bloom and native UI | Complete integrated chain; current-frame spatial Hable and an AgX approximation do not establish the selected temporal/bloom result |
| 4K output from 1080p 4× MSAA at 60 FPS VSync | Matching runtime configuration and measured warm normal-play cadence on RTX 2080 8 GB, excluding diagnostic readback stalls |
| ≤4 GiB peak lighting, including overlapping generations | Complete live/retired resource accounting and measured full-scope admission; allocation estimates and caller reserves alone are insufficient |
| ≤4 ms lighting/frame and ≤1-second update age | GPU elapsed work and actual edit-to-display/accumulation age during normal play, including scheduling and feedback |
| ≤15% brightness error and ≤1% blocked-side leakage | Unchanged meaningful masks passed by the production coarse solver, plus separate final-image fine-detail checks; the direct control's pass is not cascade qualification |

The retained `lightingSceneLabels(..., environment=true)` diagnostic branch
ignores `door_open`; it is not the new interaction implementation. The separate
Driver-owned service-door path has passed its bounded build/runtime witness as
recorded above. Showcase general dig/build/particle tools remain
gated. Reuse the Driver edit/collider/settlement facilities with revision-tagged
publication rather than bypassing them with direct grid writes. A future emitter
move must atomically replace its old/new material cells and matching immutable
radiance source; that behavior remains unimplemented.

## Recommendation and unchanged targets

Replace certified irregular-boundary tracing in the candidate frame path with
world-space transport over native simulation material cells. Preserve a cell's
material and opacity; do not average a one-cell wall into neighboring vacuum.
Retain directional radiance through refined world probes. Handle rounded visual
silhouettes, normal-map relief and short contact visibility in rendering.

The target remains RTX 2080 8 GB, 3840×2160 output from reduced-resolution
rendering and the selected temporal reconstruction, 60 FPS with VSync. Preserve
the 4 GiB peak lighting budget including overlapping/retired allocations, 4 ms
total GPU lighting work per frame, and one-second normal-play update-age limit.
The 15% brightness and 1% blocked-side gates remain acceptance criteria.

The current 69.45% aperture error is evidence about the current algorithm. CPU
and GPU C0 agreed closely; intersection precision did not fix its reconstruction.
It is not a measurement of v22. No v22 GPU performance or quality pass exists.

## What the source changes

This table compares v22 with the exact-path baseline present when reassessment
began. That baseline remains as a diagnostic. The current playable experiment
already uses native-cell direct transport and signed FP32 harmonics, as recorded
above; it has not qualified scalable cascade reconstruction.

| Responsibility | Retained exact-path baseline | V22 evidence and proposed adaptation |
|---|---|---|
| Cascade geometry | Eight base directions; angular count doubles per level. Feature receivers and scalar C0 background reconstruction use exact contour connectors. | Probe spacing doubles; angular count quadruples. Ordinary merges share a near connector across four angular descendants. Refined construction retraces each parent angular sample to a footprint exit. Test this geometry before choosing production spacing. |
| Refined representation | Contour receivers retain bins for normal evaluation; background averages C0 before sampling. | Fixed world probes retain order-4 circular harmonics: nine RGB coefficients, 27 scalars. Project using actual connector direction, then evaluate with normals. |
| Temporal work | Completed immutable generations and expensive exact-view reconstruction. | Quarter-rate coarse updates, rotating angular phase and refined EMA. Adapt these to explicit source revisions, initialization and bounded update age; camera motion should reuse the world field. |
| Sources and feedback | Catalog RGB optical data and surface emission; full diffuse feedback remains incomplete. | V22 has first-hit shading, analytic direct lights, sky and indirect feedback. Define one source inventory and explicit previous-generation feedback; do not copy implicit/stale bindings. |
| Visible fine detail | World rays solve rounded sub-cell geometry; exact-view background output has five records per input pixel. | Keep existing Grid contours, MSAA coverage and material normals. Use bounded screen refinement for visible local details; remove the per-pixel world retracing dependency. |
| Validation | Full CPU cascade mirror plus intersection/certification/precision qualification. | Keep tiny direct voxel-ray and analytic references, targeted border fixtures, GPU invariants and final-image comparisons. Do not build a second full CPU lighting implementation. |

V22's interval lengths grow approximately fourfold per level and their base
depends on map width and its penumbra parameter. For PPR, declare that base in
simulation-cell/metre units. Preserve its tested ratios while making the physical
meaning explicit; camera resolution must not change world transport.

## Active border reconstruction: what was established

The active path is [SuperSampling.update](../oni_v22.slang#L6796), not the nearby
unused `update2` implementation. It:

1. Builds a local AABB around a refined world probe.
2. Intersects each neighboring C1 ray with the footprint boundary.
3. Retraces from the refined probe, offset by `connector_direction * 1e-3`, to
   that intersection.
4. Composes near radiance/transmittance with stored far radiance before spatial
   weighting, and projects using the actual connector direction.

For refined index `(4,4)`, the literal source gives centre `(9,9)`, footprint
`[5,13]²`, parent centres `(6,6), (10,6), (6,10), (10,10)`, and weights
`(1,3,3,9)/16`. The bounded CPU float32 reproduction used opaque black columns
and injected white far radiance:

- At wall column `x=9`, 28 of 64 connectors hit and 36 escaped. A nominal +X
  parent ray became connector `(0.8,-0.6)`, starting in solid `(9,8)`; the
  opposite ray started in empty `(8,8)` and escaped.
- Inside solid columns `x=8..9`, all 64 connectors hit.
- With the wall in adjacent column `x=10`, 25 hit and 39 escaped; an initially
  empty ray could hit the wall later.

In these reproduced non-tangent cases, the nudge selects the ray-forward
starting voxel at a boundary. It is not evidence of a deliberate penetration
thickness. Starting sufficiently inside solid still blocks immediately. A
tangent component or an offset lost to FP32 rounding need not change its voxel
coordinate; this fixed epsilon is not a general boundary-robustness guarantee.
Do not enable commented rejection checks by interpretation or enlarge the nudge
to hide artifacts.

Construction visibility is only one stage. Normal-dependent harmonic evaluation
and [final bilinear sampling](../oni_v22.slang#L6849) perform no new visibility
test. In the two-column case, the probe at `(9,9)` was black, but pre-EMA final
samples at `x=9±1/16` were approximately `0.02612` and `0.07873`, both inside
solid, due to neighboring probes. Thus the active mechanism combines directional
border visibility with soft interpolation that can reintroduce light. Source
alone cannot establish whether every such contribution was artistically intended.

Frame zero actually stores black in this prototype: its adaptation factor is
one and previous history is explicitly zero. These reported nonzero samples
inspect the pre-EMA construction, not a rendered frame-zero leak. Reproduction
artifacts are under `out/diagnostics/environment-lighting-20261102/v22-reassessment/border-reproduction/`.
This is source emulation with synthetic far input, not GPU/full-cascade proof.

Scalar C0 discards angular visibility correlation before the final query.
Current contour bins retain directions but are tied to boundary-feature samples.
V22 retains directional information at fixed world probes, including the changed
arrival direction. Harmonic truncation and final interpolation still lose detail;
they require tests rather than a leak-free claim.

## Retain, adapt, remove and defer

Retain catalog materials and emitters, simulation occupancy, RGB optical
semantics, vacuum skipping, world/revision ownership, fenced generation leases,
the Driver/Editor/render lane, native UI, Grid terrain/art/normal sampling and
MSAA coverage. Preserve the old tracer and Bézier oracle as comparison tools.
The shared test application/pass and durable execution/status runner remain useful.

Adapt v22's footprint reconstruction and temporal schedule, with signed
harmonics. Its `Radiance9` packer clamps negative AC coefficients to zero; it
must not be copied as a correct directional encoding. Its refined tracer reads
C0 indirect lighting while the active supersampling dispatch omits C0. Specify
feedback as a read-only previous completed refined field, rather than inheriting
that binding. Bootstrap invalid history from the current sample. Invalidate or
refresh affected lighting after material/source edits.
The active `Cascade.probeDir` reads `Time.frame` for angular rotation even though
it accepts a `frame` argument. A port must evaluate a stored parent using that
parent's actual sample phase. The first bounded candidate can keep one phase
across an entire completed generation; partial probe scheduling must not silently
pair older radiance with a newer direction.

Remove contour-receiver generation, certified Bézier queries, FP64 retries and
the five-position exact-view background buffer from the new candidate frame
path. Do not delete diagnostic controls or unrelated exact collision geometry.
Collision, digging and building keep their existing geometry contracts.

Defer broad migration, full RGB participating-media qualification, a general
light-list system, new scheduling frameworks and further attempts to perfect the
old aperture algorithm. Add diffuse feedback only after emission/escape transport
has passed. Keep the selected Hable temporal reconstruction, exposure, AgX, bloom
and native UI requirements; existing spatial 2× output is not true TSR.

## Fine screen refinement

Use reduced-resolution depth, normal, coverage and material information from the
same rendered geometry. Flat 2D depth alone cannot distinguish a hole from solid
terrain, so coverage/material data is essential. Start with bounded short contact
visibility at silhouettes, using world directional lighting for the far input.
Avoid double-counting direct light or applying an unrelated scalar darkening to
all directions.

Offscreen transport remains in the world field. Screen misses and newly exposed
pixels fall back to that field. Reproject screen history with camera/motion data;
reject history on disocclusion, depth/normal/material changes and source revision.
Keep screen corrections out of gameplay illumination and world feedback. Fine
screen effects cannot repair a coarse-field leak through an offscreen closed door.

### Next bounded native material response

Astra's render-model recommendation is implemented as a bounded native opaque
surface experiment, after the colour-only stone A/B baseline capture. Independent
source review and the shared ordinary/GPUAV fixture passed; actual image and
cost qualification remain open. Before this change native world shading ignored
roughness and metallic; the legacy fixed highlight was not native-field specular.
Reuse the exact exposed-side CH probe already chosen for diffuse, existing
material normals/roughness/metallic/IOR and orthographic `V=(0,0,1)`. Add no world
rays, persistent targets, CPU lighting mirror or elevation-quadrature framework.

For normalized front-facing `N`, use `R=2*N.z*N-V` and the complex pair
`z=(1-roughness²)*(R.x+i*R.y)`. Evaluate signed order-4 CH using real/imaginary
parts of `z^k`; start from DC and clamp only the completed convolution. Multiply
by `saturate(R.z)`. This is a circular Poisson roughness filter with an explicit
approximate front-hemisphere response, not a GGX or full 3D BRDF integral. A flat
normal returns DC without an arbitrary reflected azimuth; below-horizon reflection
returns zero. Keep the existing flat diagnostic response flat for both lobes.

For linear reflectance `A`, derive dielectric `F0` by squaring the ratio
`(ior-1)/(ior+1)`, blend toward `A` by metallic, and use bounded Schlick Fresnel
`F` at `N.z`. World reflection is
`(1-metallic)*(1-F)*A*max(K16_diffuse,0) + F*filtered_specular`.
Fully metallic surfaces can skip K16. Retain the current mapped-normal cut-face
key unchanged and add self-emission once. Deep opaque surfaces without an exposed
probe have zero world reflection; the key remains separate from transport,
background, feedback and gameplay.

Let `H` mean the existing per-channel sum of absolute CH coefficients, and `S`
the existing maximum source bound. The filter powers and hemisphere weight have
magnitude at most one, so specular is at most `H`; K16 diffuse is below `3H`.
With `A, metallic, F` in `[0,1]`, world reflection is at most
`3H*(1-F)+H*F <= 3H`. The existing full bound `3H+S+0.04` therefore still applies
under admission `8H+2S <= 65504`, leaving ample FP32/RGBA16F margin. Do not narrow
the admitted field domain or clamp individual coefficients to implement this.

Verify constant/pole response, signed rotated harmonics and roughness decay,
horizon rejection, metallic/dielectric endpoints, zero-field key/emission and
high admitted ranges in the same shared native fixture. Preserve aperture masks
and the pure diffuse controls. Compare actual pipes and stone at identical assets,
EV, camera and door state. Collect warm MSAA4 terrain-draw cost or an explicitly
qualified paired full-frame delta; producer-field timestamps do not measure this
new work. If visuals fail, report that before adding another lighting path.

The five-file source freeze is
`native-material-response/source/freeze.json` under the v22 diagnostic root,
SHA-256 `cbad7b60363804d1d8703b91d5cf24968ecad97732b5c0a0600bfe05088d69ad`.
Astra's two test-coverage findings were corrected without production changes:
pole controls now use nonzero signed harmonics and require bit-exact DC; one
grazing dielectric case distinguishes Schlick Fresnel from constant F0. The
final test source SHA is
`e75597226d63f324ac19ed739b9cd7c34fae31feaf5005a3c3421c17a05b4e50`.
All 28 material analytic/status controls passed in the existing shared native
fixture, ordinary **14.40 s** and Core/GPUAV **18.04 s**, including setup. The
unchanged physical result remains 5.7851% small-scene L1 and zero blocked leakage.
These results do not establish MSAA4 draw cost or scalable cascades. No new
production buffers, shader layouts, queues, application or qualification pipeline
were added; original pure diffuse code and producer prefix were preserved.

The focused existing MSAA4 Grid normal/emission leaf also passed: ordinary
94.85 s leaf / 101.37 s CTest, Core/GPUAV 164.72 s leaf / 172.27 s CTest. Both
processes exited 0, with no VUID/validation errors. The actual v4-root door run
then completed its blocking, stability, traversal, occupied-door rejection and
final-closure witness. The coordinator inspected its two losslessly verified 4K
frames; the background shadow holdout is unchanged, but pipes still read flat.
This does **not** establish final art acceptance or justify adding another
lighting path. The process again exited 1 on the existing libdbus LSan class.
Artifacts are in
`native-material-response/runs/native-material-world-only-v4-20261103/` under
the v22 diagnostic root.

Two separate warm whole-frame timing runs used the same binary and v4 assets,
with only the Grid shader's native-material literal false/true. **Both windows
are incomplete: 209/256 samples, interrupted for optical revision immediately
before orderly shutdown.** Keep the original 256-sample requirement; no retry or
completed paired-performance claim follows from these runs. Partial false/true
means were 7.2847/7.5089 ms, p95 7.3175/7.5582 ms. These describe only the retained
partial whole-frame samples, not isolated terrain/lighting cost, physical cadence
or 4K60 acceptance. Ready-result receipt gaps also exclude other frames and do
not establish application/display FPS. See
`native-material-response/runs/metrics-ab-20261103/`.

One bounded source diagnosis now points to authoring rather than another BRDF:
the existing 128² pipe exporter multiplies periodic height differences by 5.
For its tube amplitude 0.30 and a cylinder radius half the tile period, physical
central derivatives require `(128/2)*(0.5/0.30)=106.6667`, a **21.3333× slope**
ratio before quantization/dead-zone handling. This is not a normal-angle ratio.
The entire same height field would also imply 61.1 mm collar relief and 1.67 mm
scratch peak-to-valley at period 1/3 m; do not silently exclude or retune those
terms. Texture/contour centres agree, so no UV/green-channel flip is indicated.
The across-cylinder periodic seam is not differentiably smooth. Actual decoded
normal distributions and the existing-stencil byte match were then checked:
both original PNGs exactly match the old generator, with normalized Z mean
0.995797 and median 0.999071. The diagnostic is
`native-material-response/runs/metrics-ab-20261103/artifacts/pipe-normal-map-diagnostic.json`,
SHA-256 `60c4bed5546c1c1e9cd9733a9f3a8b24a3f8bad2b0678dd7e4026c0832123b37`.

The bounded full-height physical derivative trial passed its CPU/export checks
and changed only the two pipe normal images in an independent runtime root.
The actual closed/open door witness passed. Both lossless 4K frames were inspected:
the pipes now read as cylinders with visible collar bands and coloured reflections.
The predeclared pipe holdout changed in 23.16% of pixels (mean absolute RGB-channel
delta 3.977); stone and background-shadow holdouts were byte-identical. This is an
accepted incremental pipe appearance correction, not reference-level environment
or motion/mip/performance acceptance. No pipe-specific prepared-image/mip rows were
logged, so do not infer sampled GPU normal/mip measurements from the PNG audit.
The game still exits 1 on the existing libdbus LSan class. Evidence:
`native-material-response/runs/pipe-normal-physical-door-20261103/frame-and-physical-normal-summary.json`,
SHA-256 `7577ebeb4f8eb75dda2f8408dcae2d2cd97dea4dbd11dc8c93c6b17e0bfe4a20`.

The two accepted normals and the matching **pipe-only** derivative correction
are now in the canonical pack/generator. All other material derivative factors,
sampled heights, albedos, descriptors, geometry, optics and emission remain
unchanged; the faceted v4 stone was not promoted. The canonical generator was
checked against preserved original height traces and the accepted output PNGs,
including an unchanged housing control. These semantic checks exited 0; report:
`material-lookdev/pipe-normal-physical-authoring/source-promotion/semantic-check-20261103/semantic-equivalence.json`,
SHA-256 `a35c11d5c2358a103e86f07396490731a33e7b15c0359f86244b082e4e0d6a3f`.
The normal `app.game` staging build exited 0; its binary remains
`01171bd09ea257bf88bb33f50991fe7010b14b6e808dfa2cc78f81d48449c990`.
All 682 game-staged worldgen files match canonical source, and staged shaders
match the reviewed true-material source. Preserve the current lighting
implementation while full material/art and performance qualification remain open.

### Reusing expensive validation setup

The existing spatial normal/emission leaf already shared an application and
GridPass, but its capture helper recreated two modules, three programs and three
helper pipelines for each of six controls. A fixture-local owning family now
serves all six captures: three helper pipeline requests rather than eighteen.
Roots, targets, uniforms, rendering, fence/readback guards and all pixel assertions
are unchanged. Other callers retain private-family behavior; no global cache,
production ownership change, new application, test registration or longer timeout
was added. Astra ownership review, author post-format audit and format/style pass.

The same ordinary/Core-GPUAV entries passed after the test-only change, in
80.43/123.85 s CTest versus the earlier 101.37/172.27 s observations. This is one
run per configuration, not a repeated causal benchmark or universal speedup.
Both execute the same six controls under the existing 300 s deadline. Current
test source SHA is
`0cee49cbe4da3b26b1ddeb2c976fd3ebf2c6e8b561ebc76e6577cec0c3989c9e`;
test binary SHA is
`4e91381788df9646b6de0c3afdf730ba26349cd04781a085ad53b8f52ecc4c5f`.
Records are under `spatial-fixture-setup-reuse/execution/` in the v22 diagnostic
root. No reported VUID/validation errors occurred, with Core and GPUAV enabled
for the validation twin. This reduces repeated setup in this leaf; it does not
claim that all test pipeline duplication has been removed.

## Storage and work: estimates, not measurements

At 4096² native cells:

| Layout | One generation | Two generations |
|---|---:|---:|
| Literal v22: six 8-byte interval slabs + 32-byte refined record per four cells | 896 MiB | 1.75 GiB |
| Candidate: five 16-byte slabs + 64-byte signed refined record per four cells | 1.5 GiB | 3 GiB |
| Five full 32-byte FP32 RGB L/T slabs + the same refined record | 2.75 GiB | 5.5 GiB |

The 16-byte option reuses the existing packed interval layout: six binary16
values for independent RGB L and T, plus status; composition remains FP32.
Overflow remains an explicit failure. The 64-byte refined estimate budgets 27
signed binary16 coefficients, padding, status and validity/revision. It needs
signed range/encoding tests; the existing nonnegative interval encoder is not
directly reusable for negative coefficients.
Keep an FP32 harmonic control in the tiny experiment before qualifying the
compact signed representation. The compact full-world numbers are layout
estimates, not claims about the first experimental allocation or measured use.

Two u16 material-ID grids cost 64 MiB; two existing u32 hierarchies cost about
170.7 MiB. After those and two packed fields, about 789 MiB remains within 4 GiB
for palettes, extra history, scheduling, staging, alignment, screen refinement and
retired allocations. Two expanded 40-byte source grids alone would consume
1.25 GiB and break this example budget. Use IDs plus a palette and prevent an
unchecked third full-field allocation. Whole-device 8 GiB admission must also
include display/TSR and other game resources.

A literal full-map v22 dispatch is estimated at roughly 268 million refined
connectors plus 71 million coarse/top queries per frame. Compact storage does
not establish 4 ms execution. Begin with the bounded scene, then measure a fixed
world-region work quota. Full-world scheduling follows only after measured
quality/work evidence; its age must include queueing and temporal accumulation.

The measured 421–528 ms background reconstruction and 69.45% aperture error
belong to the old implementation. No speedup or full-world v22 timing is claimed.

## Smallest playable experiment and short validation

1. Add one candidate shader/backend in the existing lighting pass infrastructure:
   native-cell occupancy, existing material IDs/palette, v22-style directional
   world refinement and explicit world intervals. Start with opaque emission and
   declared sky-on-escape; no feedback or screen refinement initially. Keep the
   old path selectable as a diagnostic control.
2. Use the existing catalog-backed 64×36 cavern in the real supported simulation
   grid, with verified vacuum exterior. Reuse its actor/colliders, camera, UI,
   generation leases and completed-picture handling. Route one door toggle and
   emitter move through Driver-owned edits. Do not build a separate viewer.
3. In one shared application/device/pass per validation policy, run constant-field,
   one-cell wall, unchanged closed/open aperture and emitter-move cases. Use tiny
   direct native-cell ray references and analytic cases, not a CPU cascade clone.
   Keep ordinary and GPUAV separate processes. Retain durable phase/exit records.
   Check the far record's interval origin against the footprint endpoint; the
   geometric reconstruction is an approximation to validate, not an exact-path
   concatenation proof.
4. Add one short screen refinement pass using current Grid coverage/normals and
   compare world-only versus refined final images with fixed exposure and sources.
   Inspect inside/face/empty-side samples, normal response and readable backwall
   shadows. Use one final-image fine-geometry reference for those visible details.
5. Walk/pan/zoom, reveal previously offscreen terrain, close/open the door and move
   the emitter. Measure warm GPU lighting time, all live bytes and actual
   edit-to-display/history age. Reuse existing in-game acceptance hooks for
   scripted camera/lifecycle evidence; report native-input latency unmeasured
   where no supported input automation exists.

Do not proceed to a full-world rewrite before this result. A small-scene pass
does not establish 4096² or 4K60 qualification.

## Quality-gate interpretation requiring an explicit decision

Using native occupancy as the coarse transport reference changes the former
rounded-Bézier world-space reference. Make that change explicit in the experiment.
Retain the same meaningful wall/door/source masks and 15%/1% numerical limits for
coarse transport, and a separate final-image fine-silhouette check. Do not replace
the aperture with an easier scene, grade screen-corrected pixels as proof of
offscreen correctness, or silently exempt a border band. Visible surface lighting
inside a rounded edge and light leaking into the empty room behind a closed wall
are different phenomena; define their masks before collecting acceptance results.


**Descriptor experiment retention decision:** the approved batching experiment is
withdrawn:4.632ms recorded API savings across the capture and no end-to-end gain
are insufficient to justify the private overlay. Source review passes; registered
world ordinary/GPUAV tests pass40.12/48.02s after removing an unintended wrapper
slow-unwinder override. Preserve the earlier timeout evidence. Original descriptor
source registration is restored; build restoration is tracked in the descriptor report. Ordinary asset passes
252.95s; asset GPUAV times out at300.31s and remains unqualified. This does not complete the full lighting plan.
