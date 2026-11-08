# Emissive temporal radiance cascades with optional sky

Status: architecture explicitly selected by the user on 2026-11-06 in the side
conversation; this concrete design is a REVIEW DRAFT. Implementation remains
paused until user validation/resumption in the main session. No implementation
or performance qualification is claimed by this document.

## Replacement objective for the main session

Finish PPR's intended V22-inspired lighting pipeline: world emissive materials
and cells plus optional last-cascade sky/time-of-day are the light sources; a persistent world-space radiance-cascade
field is the sole production illumination solution, updated with bounded work
across frames and previous-field feedback for approximate multiple bounces.
Start with4096² stress, expose coherent RC resolution/spacing/level/interval
quality controls, and retain screen-space micro traces for visible fine misses.
Integrate terrain, supported dynamic Matter, world edits, directional material
shading and existing temporal/HDR/bloom/UI presentation. Retain straight tracing
only as a bounded diagnostic oracle. Qualify representative optimized performance,
memory, temporal response and user-approved visual approximation; measure
validation separately. Preserve the remaining applicable original-plan scope.

This replaces the interpretation of “finish the implementation of the plan” that
promoted direct_window tracing and explicit face-moment baking into production.
It does not authorize automatic resumption of a user-paused goal.

## User decisions and authority

- RC is required production architecture, not an optional optimization candidate.
- Local sources live in the world as emissive materials/cells. No analytic lamps,
  point/directional light lists or separate direct-light pass. The user explicitly
  permits optional sky sampling in the LAST cascade for inexpensive time of day;
  this supersedes the earlier no-sky restriction. Sky is not an analytic sun lamp.
- Ignore V22's Shading::directLighting branch. A visible emitter's own material
  emission remains valid and must not be counted twice as incident illumination.
- Retain screen-space micro traces to correct visible fine-detail ray misses,
  as the user explicitly clarified. This is required local visibility/refinement,
  not a competing world GI solver or new light source.
- Expose coherent RC quality controls: probe-field resolution, spatial spacing,
  angular samples per probe, cascade count and per-level near/far ray intervals.
  These are quality/performance levers, not fixed oracle-resolution requirements.
- Temporal feedback is the intended approximation to multiple bounces. No explicit
  discovered-face moment bake or recursive per-frame path tracing in production.
- The user accepts approximation error for bounded-cost world GI. Historical
  15% brightness /1% leakage thresholds become diagnostic measurements, not
  architecture-selection vetoes. New visual acceptance is reviewed on captures;
  do not invent replacement percentages or silently remove safety checks.
- Delivery priority: finish the playable pivot today,2026-11-06. Use the revised
  four-task execution plan and three consolidated validation units. Do not grow a
  new test framework or broad qualification project before integration. Deadline
  is a target, not permission to claim unmeasured budgets or missing behavior.
- Start runtime implementation and profiling at4096²: the user explicitly designates
  this as STRESS mode. Small fixtures are correctness tests, not the first product
  target;1024²/2048² normal cases follow the4096² baseline.
- Preserve RTX2080/8GiB, normal lighting6ms/stress10ms, peak lighting4GiB including
  retained owners, 4K output from1080p/MSAA4 and60Hz targets unless changed by user.
  Keep the1s response target, but measure source response and multibounce settling
  separately. The proposed temporal metric definition below needs plan validation.
- No RHI migration or new dependency fork. Slang remains the shader language.

## Chosen approach and alternatives

Adapt the active V22 cascade/refinement/feedback structure inside existing
WorldRadiancePass ownership. Reuse PPR's native source/material traversal,
Matter projection, fences, immutable publications and directional consumers.
This follows the user's explicit selection; no further RC-versus-direct choice
is pending. Merely renaming cascade_diagnostic would omit temporal GI state;
continuing the direct/face-bake hybrid conflicts with the requested result.
A wholesale prototype transplant would lose PPR ownership and error contracts.

## Current state and useful source anchors

- Editor initial/replacement settings select direct_window, with256/512 quotas.
- world_radiance.slang::worldSolveCascade/solveWorldCascade and the cascade branch
  of worldRefine survive, but are custom adaptations with failed historical tests.
- refineWorldRadianceParallel is full-length256-ray receiver tracing, not RC.
- Face discovery/bakeWorldFaceMoments[Parallel]/secondary cascade is a separate
  hybrid bounce pipeline. Preserve its current uncommitted edits as experiment
  evidence; do not continue promoting it as the production solution.
- Current WorldRadianceCoefficients is112B signed FP32 per receiver. GPU source
  data, native DDA, Matter projection and render-owner lifetime rules are reusable.
- Existing temporal_2x history is image reconstruction, not GI feedback.

V22 anchors in docs/oni_v22.slang: Cascade~6111; quarter-probe update~6424;
probeToUpdate~6274; coarsest/parent merge~6320/6360; active SuperSampling.update
~6796 (not update2); RadianceMultiBounce~9561; pass ordering~9564–9597.
Its refined field is order4 CH; cascade samples pack into8B and Radiance9 into32B.
Prototype MAP_SIZE is1024; production starts at4096² stress, so copying its
capacity/interval assumptions is insufficient. Prototype defaults are learning_rate1 and penumbra_width0.5.
V22 probeDir accepts a frame argument but reads global Time.frame: do not copy
this ambiguity into staggered stored probes. Record the phase used by each slot.

## Proposed production dataflow

World optical source (terrain + committed Matter; optional sky at last-cascade
terminal boundary) → bounded near/far RC interval
updates, coarsest first → directional refined world field → world consumers.
On a ray hit, outgoing radiance is material emission plus material-reflected
irradiance sampled from the previous completed GI field. Evaluate one previous
field, not an explicit ray tree. Absorption/transmission and participating emission
continue through the existing canonical material projection. No refraction is added.

Separate three histories: cascade sample state, world-space GI feedback/EMA, and
screen-space TSR. A camera move changes screen history, not world-source lighting.
EMA alone cannot create bounces: feedback must enter the hit shading calculation.

### State, phase and publication

Use explicit previous/read and next/write ownership in the pass. Initially black,
initialized history is distinguishable from unavailable/invalid data. A field
published to consumers is never overwritten while leased or in flight. Retain
existing prepare/encode/notifySubmitted/advance/acquire failure and fence semantics.
Pass-owned internal slots may evolve between publications; every lease references
an immutable accepted version. Avoid allocating a fresh whole-world field per frame.

Use V22's four spatial update phases and order4 refined coefficients as the
initial measured configuration, not immutable quality targets.
Each sample records the angular phase of its last accepted update. Parent reads
use that phase, not the current global frame. Advance phase only on accepted GPU
submission; discard/failure does not consume it. No same-buffer cross-workgroup
read/write feedback dependency. Coarse-to-fine dependencies have explicit barriers.
A source revision cannot relabel old radiance as fresh; retain a distinct dirty/
responding state until the affected scheduled work runs. Coalesce new edits without
starving old dirty regions. Topology changes invalidate incompatible feedback.

For the first verified implementation, a source revision may conservatively reset
world GI history; its bandwidth and visible recovery are measured. This is a
correctness baseline, not permission to ship a permanent full reset on every
Matter tick. The integrated edit/Matter sequence must qualify bounded affected-region handling
before integration is accepted. World replacement/device reset always resets incompatible history.

### Coverage, packing and bounded work

Cover the declared world, including offscreen emissives/occluders. Do not quietly
retain camera-only source or lighting coverage. Fixed work budgets bound dispatch
records per actual frame; update coverage and age remain explicit.

“Constant time” means predictable work for a chosen world resolution/update budget,
not O(1) in map area, cascade count or ray traversal length. Derive storage and
work at1024²/2048²/4096² before full-world allocation. Derive interval reach from
world extent; do not copy six levels and assume they reach all4096² sources.
Keep interval units in cells/metres; a camera zoom must not alter light transport.

For today: retain existing112B signed CH if a measured coarser profile fits the
budget; avoid making a new32B CH codec a prerequisite. V22's8B sample packs RGB radiance plus ONE scalar transmission value; it is not
an ABI-compatible replacement for PPR's RGB radiance/RGB transmission. Preserve
coloured transmission with the existing representation or an explicitly budgeted
compact RGB L/T encoding (including required status/validity). Select this in Task1;
8B is an example, not a required format or permission to discard colour absorption.
Any changed encoding requires the one focused layout/round-trip subcheck.
A new CH codec is conditional optimization, not mandatory work. Tiny fixtures
supply controls while the first runtime target remains4096² stress. Compare V22's8B/32B storage to explicit PPR ABI; do not simply cast
existing112B records. Count previous/next and leased/retired copies, sample-phase
metadata, source/Matter, upload and scratch allocations. Include decode cost.
Choose the measured4096² configuration against the10ms STRESS budget, then
qualify normal1024²/2048² cases against6ms; if none fits,
report the RC feasibility gap and change RC resolution/update design explicitly,
never fall back to production straight tracing.

### Quality controls and screen-space micro correction

A named quality profile declares world-field resolution/coverage, spatial probe
spacing, angular resolution, cascade level count and interval starts/ends. Field
resolution and spacing must resolve to one consistent grid rather than conflicting
knobs. Keep refinement resolution distinct from angular samples and update cadence.
Report the resulting records, memory, far coverage and sample age for each profile.
Validate monotone interval boundaries, valid angular parent/child mapping and
complete declared coverage. Quality changes invalidate incompatible history; do
not silently reuse coefficients from a differently phased or spaced representation.
V22 ratios/defaults are a baseline, not a prohibition on measured quality tuning.
“Ray internals” is interpreted here as ray intervals (near/far ranges) and their
sampling parameters; any additional intended control can be added during review.

Retain/adapt the existing screen-contact/fine-refinement work where it fits the
user's micro-ray correction mechanism. Screen traces use visible fine geometry,
material/normal data and RC radiance to resolve local visibility misses. They add
no analytic lighting source and must not double-count world emission/RC energy.
If correction samples emissive surfaces, document its replacement/composition rule
rather than adding a second unaccounted lighting estimate. Bound screen trace
length/work, preserve a defined RC-only fallback when screen evidence is missing,
and test moving geometry, disocclusion, thin features and screen edges. These
traces cannot establish offscreen visibility or repair the world field for gameplay.
Include their GPU/memory cost in total lighting. Visual relief/art matching remains
separate from correctness of this required mechanism.

### Conditional optimization policy (explicit user clarification)

Start with4096² stress and measured baseline. Temporal amortization is architectural;
additional tuning is evidence-triggered. Candidates, only when needed: time slicing
and ray budgets; conservatively skipping completely occluded probes; faster
bitmask/jump ray marching (consider Vulkan R8UI where supported); thread-group ID
swizzling for L2 locality; wave intrinsics/lane indexing. Do not conflate wave-local
indices with dispatch/thread-group swizzling. Do not implement them all upfront.
Each candidate names a measured bottleneck, preserves/emits its numerical and
coverage impact, runs a matched comparison and is retained only for material gain.
An enclosed probe can still matter for emissive/participating material, future
edits and indirect propagation; prove its skip eligibility and invalidation.
Keep the actual per-frame GPU bound and maximum update age visible together;
lower dispatch cost purchased by unbounded stale light is not success.

### Temporal response and acceptance proposal

Measure t0 at accepted world edit; t_response at first matching RC field consumed
by a presented frame; and t_settle against that SAME RC configuration's stabilized
result. Require t_response<=1s as the existing freshness target. Never call a
merely new source tag or untouched stale field a response. Record regional response/decay through the shared GPU/playable sequence, including
emissives switching off. No mandatory300-frame plotting framework is required. No fixed multibounce convergence threshold is asserted
without user visual validation. High-albedo closed scenes must remain finite,
bounded under the chosen feedback model, and decay after emission removal.

Preserve reference errors as reports. Separate historical oracle qualification
from production RC acceptance, so relaxed visual approximation does not weaken
payload correctness, out-of-bounds, numerical validity, lifetime or validation tests.
Cover these as substeps of one shared fixture/playable sequence, not independent
expensive fixture runs. Representative visual matrix: open/closed door, narrow aperture, emissive motion/
on-off, coloured walls, high-albedo enclosure, camera pan/zoom, dynamic Matter and
world edges. Compare against both direct oracle and emissive-only V22-style RC,
showing differences rather than treating one sparse point fixture as sole truth.

## Reuse, retire and unfinished original scope

Reuse native source/material data, safe traversal, canonical Matter projection,
world edit transactions, resource leases/fences/cancellation, signed directional
shading, image reconstruction/HDR/bloom/native UI and diagnostics. Retire production
straight256/direct_window and explicit face-discovery/bake orchestration only after
RC integration passes. Keep bounded reference tests and historical evidence.

Gameplay must sample the authoritative RC field through value-only snapshots;
the existing CPU optical-ray diagnostic is not a production GI substitute. Preserve
original supported media/gameplay requirements in the acceptance ledger. Any
undefined gameplay consumer or additional scattering model needs a named bounded
follow-up, not an invented mechanic or silent completion claim. Visual art matching
remains previously deferred. Windows validation remains distinct from Linux.

## Validation and resumption decision

User validates this revised design and companion four-task implementation plan in
the main session. A brainstorming builder self-review was performed (side-chat
subagents prohibited). The original eight-task plan was not a credible same-day
commitment: the revised plan reuses code, consolidates checks, postpones optional
codec/tooling/cleanup and detects ownership/performance blockers early.
That validates proposed temporal metrics/packing sequence, not a claimed GPU result.
No speculative performance or deadline guarantee, blanket source rollback or
automatic commit. Only3 consolidated validation units are planned: CPU/layout/state,
shared GPU sequence ordinary+GPUAV, and4096² playable sequence. Existing valid
evidence is reused; further tests require a changed contract, actual failure or
unresolved correctness risk. Broad original-goal gaps stay explicit, not waived.
The main goal must reference this objective and both artifacts on resumption.


## Last-cascade sky rule (latest user decision)

Sky radiance is an optional terminal boundary input to the coarsest cascade.
Time of day is supplied explicitly by the application and affects that sampling;
lower cascades transport it through normal L/T composition. A finite interval miss
is not sky, and an enclosed ray must not acquire sky through a wall. Test sky-off
blackness, sky-on open boundary, enclosure and time-of-day change in the same GPU
sequence. No separate shadowed sun/point-light pass is authorized. Sky changes
participate in normal temporal updates rather than resetting world GI every frame.


## Main-session brainstorming review — 2026-11-06

Architecture matches the user's chosen result. A fresh Sol builder review found
one consequential product ambiguity: a mixed-age field can produce a first changed
pixel long before all affected visible receivers have been updated. The proposed
clarification, pending user answer, is that1s covers a genuine post-edit transport
update for every affected visible receiver (and explicitly requested gameplay
receiver), while multiple-bounce settling is measured separately. Merely publishing
a source tag or one changed probe does not satisfy that proposed response metric.

Engineering corrections: do not impose V22's8B scalar-transmission format on RGB
media; budget the actual selected representation and all retained owners. Task1
must yield a concrete memory/work/ownership profile before product allocation.
Do not spend the same-day window implementing optional compression first.

At19:25 CET the five-hour sequential timeboxes exceeded the4h35 remaining today,
even before contingency. Therefore today remains a delivery target, not an honest
five-hour completion forecast. Do not squeeze away feedback, world coverage or
safety to make the schedule appear to fit. A concrete ownership/admission blocker
or failed first4096² budget must trigger an immediate factual update.

Slot count, deterministic phase handling, feedback-version cadence and bounded
material response are engineering choices, not additional questions for the user.
The three validation units remain unchanged. No production files/build/runtime
were changed by this review, and implementation stays paused.


## Approval and resume

User approved the revised plan and resumed implementation2026-11-06.
The1s target requires an actual post-edit update across the full affected visible
region and explicitly requested gameplay receivers. Multibounce may settle
progressively. This resolves the review question above. Earlier REVIEW DRAFT/
paused wording is historical; this approval is current. Actual resume21:05CET:
less than3h remain today, not the earlier5h estimate. No missing feature or gate
may be relabelled complete to meet that target.
