# Spacing-4 radiance cascades with deadline-aware updates

Status: approved for implementation by the user’s explicit resume instruction
on 2026-11-07. Spacing remains 4 with six configured levels (refinement plus
five traced cascades), including on 4096² maps. No new performance qualification
is claimed.

This revises the scheduling, traversal and resolution choices in
[the temporal RC design](2026-11-06-emissive-temporal-rc-pivot-design.md).
All other applicable architecture, integration, safety and acceptance obligations
remain. The earlier spacing-16/32/64 experiments are evidence, not production
quality choices. This document supersedes those choices and the fixed quarter
update schedule. It does not replace the entire implementation plan.

## Intent and accepted requirements

Keep the refined world probe spacing at **4 native cells**. Preserve the cascade
spacing progression above that base; this does not mean every cascade has spacing
4. Cover the full world, including offscreen sources and occluders. Retain world
emissives, optional terminal sky, previous-field multibounce feedback, directional
shading, canonical Matter, screen micro correction and existing presentation.

The scheduling priorities are, in order: correct post-edit transport within the
one-second deadline, then convergence/variance-guided work, with bounded background
refresh to prevent starvation. Multiple bounces may settle progressively. A
publication tag, first changed pixel, or a new fine sample with stale required
parent data cannot establish deadline completion.

Keep the existing total lighting limits: normal 1024²/2048² <=6 ms, 4096² stress
<=10 ms, peak lighting ownership <=4 GiB on the RTX 2080/8 GiB target. Preserve
the existing output/frame goals. There is no approved budget relaxation or
automatic spacing increase when overloaded.

## Evidence and limits

The current implementation dispatches all cascade levels and refinement for one
of four spatial parities in a frame, publishing after all four. In the corrected
4096² Release captures, spacing 16 measured median 51.183344 ms and maximum
61.50224 ms per phase; spacing 32 measured median 14.342256 ms and maximum
20.844544 ms. These are RC phase timings, not total lighting or total frame cost.
Both fail the stress limit. They do not predict spacing-4 throughput reliably.
Artifacts live under `out/diagnostics/windowed-native-20261106/temporal-rc-pivot/`
in `corrected-grid-spacing16-4096` and `corrected-grid-spacing32-4096`.

Parallel refinement already preserved serial summation order in the shared GPU
test and materially reduced its measured cost. Current traversal still walks
vacuum hierarchy levels during ray marching. `docs/oni_v22.slang` uses occupancy
bitmasks and `passMapSkipDist`, whose R32UI output contains only 0,1,2,4,8,16,32.
Those values fit losslessly in R8UI. A single 4096² table has 16 MiB raw texel
payload in R8UI versus 64 MiB in R32UI, excluding allocation overhead/extra owners.

At spacing 4, the current padded 4096² refined field has 1026² records. Its
112-byte coefficients alone require 117,899,712 bytes per slot, before cascade
storage, three-slot ownership, source data, scheduling metadata and scratch.
Recompute complete live/retired peak admission before a full allocation. Do not
silently introduce packing or drop colour transmission to make admission pass.

## Approaches considered

1. **Fixed rotating fraction:** simple coverage bookkeeping, but cost varies by
   cascade and geometry, and unchanged regions consume the same quota as edits.
   Retain only as a diagnostic comparison; one quarter is not a hardware law.
2. **Variance-only priority:** can reduce convergence work, but stale unobserved
   probes can appear stable and never wake. Does not establish edit deadlines.
3. **Chosen: deadline-aware, cost-bounded scheduling with variance and refresh.**
   Explicit dirty/version tracking handles edits; variability spends remaining
   capacity where useful. GPU queues preserve locality and avoid CPU per-probe
   readback. Complexity is justified by the measured frame-budget failure.

Evaluate conservative R8UI traversal acceleration first. It changes the cost of
the work the scheduler must budget; quotas chosen before that measurement would
be provisional guesses. This is an experiment to qualify, not an assumption of
speedup or a requirement to retain an unsuccessful implementation.

## First evaluation: R8UI skip table

Reuse the canonical optical source and occupancy information. A table entry
describes a certified empty aligned block usable by the DDA, not an arbitrary
distance along the ray. Advance to that block's exit boundary and preserve exact
entry-face, corner, interval-end and world-boundary semantics. Do not copy V22's
epsilon rules without checking PPR's existing traversal contract.

Empty means no surface coverage, extinction, participating emission or other
supported optical interaction. Terrain and committed Matter both participate;
partial coverage is not empty. Quantization may shorten a safe skip, never enlarge
it beyond the certified empty block. Values remain the verified discrete set.

Query Vulkan sampled/storage-image format capabilities as required by the actual
build/read path, and check Slang-RHI binding support. Use typed integer loads, no
filtering. If unsupported, report the result; do not add a new RHI or dependency
fork. R32UI can remain an experimental control, not a silent claimed R8UI result.

The table and optical source must describe the same accepted version. A new
obstacle invalidates every affected block certificate, including entries outside
the edited cell that could skip across it. Rebuild the affected block support
before using the new source. If precise incremental maintenance is not yet
available, rebuilding the table for that frozen source is the correctness
baseline and its full cost is charged. Never reuse optimistic stale skip data.
Removing obstacles may conservatively retain shorter skips until rebuilt.

Compare existing traversal and R8UI traversal using identical spacing-4 sources,
ray inputs and shader quality. Include empty/open, enclosed, thin/corner geometry,
emitter changes and moving/partial Matter. Compare hit identity, RGB L/T, finite
status and progress; boundary changes are failures, not visual approximation.
Measure table construction/update, traversal, barriers and retained memory, both
steady state and edits. Retain acceleration only if it materially improves the
complete workload without correctness regressions. A smaller table alone is not
evidence of faster transport.

## Frame budget and CPU/GPU responsibilities

C++ supplies the injected clock, accepted edits and source versions, demand
regions, memory admission and the total frame allowance. GPU passes classify
work, maintain compact priority bins and produce bounded indirect dispatch lists.
Read back aggregate timings/backlog statistics asynchronously; no per-probe CPU
sorting, same-frame readback wait or full-world CPU scan each frame.

Budget **all lighting**, including source/Matter preparation, skip maintenance,
queue work, ray transport, field refinement/publication and screen correction.
Subtract estimated non-ray costs and measured headroom before assigning ray work.
If there is no remaining allowance, do not issue optional convergence work.

Maintain cost estimates per cascade and work class using completed GPU timestamps
and work counts. Use conservative recent costs, bounded quota changes and a small
initial dispatch to calibrate spacing 4. Expensive long intervals and refinement
must not share a single global rays-per-millisecond estimate. Maximum dispatch
size and traversal progress bounds limit exposure to estimation errors.

This is a predictive controller, not GPU preemption: already submitted work cannot
be stopped at exactly 6/10 ms. Log overshoots and reduce subsequent quotas;
qualification still requires measured compliance. Initial quota, headroom and
cost-estimator constants are implementation choices established by measurements,
not newly invented product thresholds or a promised universal ray count.

Use spatial tiles and per-cascade work items with stable ordering inside priority
bins. Prefer complete directional samples for selected probes initially, keeping
the existing integration order. Splitting a probe's angular set is a later option
only if needed, with explicit partial-estimate and phase bookkeeping. No broad
global sort or new generic job framework is required.

## Deadline and dependency scheduling

Track accepted edit time, source identity, dirty state and last completed transport
version separately from last sample time and convergence statistics. Coalescing
edits keeps the oldest outstanding deadline while targeting the latest frozen
compatible source; constant Matter changes must not reset the deadline indefinitely.
An in-flight source snapshot remains immutable and finishes within a bounded
service interval; newer edits remain pending rather than repeatedly cancelling it.

The deadline covers every affected visible receiver and explicitly requested
gameplay receiver, including the offscreen cascade dependencies needed to update
them. Full-world source/field coverage remains; it does not add a new requirement
to refresh every untouched offscreen receiver once per second.

Invalidation includes geometry/transmission changes, emitter changes and sky.
Propagate dirty state through the actual cascade sampling support, including
connector ray intervals; a local edit is not assumed to have only local lighting
influence. If conservative support cannot be bounded tightly, widen invalidation
up to the full relevant dependency closure. Never substitute camera visibility
or unchanged old coefficients for a proof that transport is unaffected.

Service required parents before their fine consumers, with version-aware readiness
and explicit barriers. Work is selected by approaching deadline first; within
equivalent urgency use locality and measured cost. Reusing a parent is allowed
only when it is valid for the edit or conservatively proven unaffected. Track the
transport dependency version separately from the previous-field feedback version:
new source visibility/emission must propagate, while later reflected bounces may
still converge progressively. EMA/history must respond to invalidation rather than
masking emitter-off or topology changes indefinitely.

Measure deadline completion from accepted edit to consumption of matching updated
coverage by a presented frame, or delivery of the requested gameplay result.
Include source preparation, queue waits, parent updates, refinement and publication
latency. Do not stop the timer at dispatch or fence completion alone.

Estimate mandatory remaining cost against time left. Optional work yields when a
deadline is at risk. If mandatory work exceeds available capacity even then,
report overload/deadline failure explicitly. Never silently exceed the GPU budget,
increase spacing, narrow demand or claim unchanged samples fresh. Worst-case
global edits are a required feasibility measurement; variance cannot make their
first post-edit update disappear.

## Variability and non-starvation

After mandatory work and a bounded refresh reserve, allocate the remaining budget
to expected improvement per estimated cost. Use temporal changes in radiance/
directional coefficients with an absolute floor and relative normalization so
dark noise does not dominate. Preserve chromatic/directional changes; luminance
alone can miss a coloured or rotated lighting change. High angular contrast at a
stable shadow edge is not itself evidence of temporal uncertainty.

Keep source-change invalidation distinct from statistical variability. Reset or
discount statistics on incompatible edits. Account for elapsed time/sample count
when comparing sparsely updated probes and when applying EMA. Record the angular
phase actually used; never reinterpret old samples using the current frame phase.

Reserve work for aging active regions even when their measured variability is low.
Use a bounded rotating refresh policy and age escalation; select its service bound
from measured capacity and document it, separately from the hard one-second edit
deadline. If mandatory work consumes the reserve temporarily, carry refresh debt
and expose starvation/age rather than pretending unobserved probes are stable.
Offscreen regions remain eligible because they can illuminate visible receivers.

## Conservative inactivity classification

Occlusion from the camera, darkness, or being in a closed room never independently
permits skipping a probe. A cascade ray may start beyond its probe's containing
cell; a coarse probe inside solid geometry can still contain useful intervals.

Initially skip only work proven to have no contribution under its actual sampling
support and consumer dependencies, or work already valid and not due for refresh.
Opaque-interior tests are eligible only with that proof, excluding emissive/media
contributions and required interpolation support. Uncertain cases remain active.
Geometry/Matter/emission edits invalidate the classification's support and wake
work before relying on it. Keep cheap classification separate from expensive rays;
charge its maintenance to the frame budget. More aggressive inactivity heuristics
are explicitly outside this revision until measured and validated.

## Publication, ownership and scope

Reuse WorldRadiancePass and its accepted-submit/fence lifecycle. A rejected or
discarded submission consumes neither work nor phase. Never overwrite a field
leased by Grid, gameplay or in-flight feedback. Preserve unchanged regions when
publishing sparse updates; do not accidentally publish uninitialized pool slots.

The existing three-slot scheme is the starting facility. Account for any whole-
field preservation copies at spacing 4 and avoid allocating per frame. Regional
completion metadata must distinguish fresh updated coverage from retained history.
If immutable publication copying becomes a measured blocker, escalate a concrete
ownership design revision; this document does not authorize a bespoke replacement
storage system. Preserve failure, cancellation and clean teardown behavior.

No analytic light lists, direct256 production fallback, face bake, new RHI, new
denoiser project or coefficient codec is part of this change. Variance-inspired
scheduling does not require implementing SVGF image filtering. All prior required
terrain/Matter edits, gameplay field sampling, sky/time-of-day, micro correction
and presentation integration remain outstanding until verified.

## Compact validation and delivery

Reuse the existing CPU case, shared GPU scene sequence and playable captures.
Add focused subchecks for table/source coherence, dropped-submit accounting,
deadline retention under edit coalescing, dependency readiness, stable-dark wakeup,
colour changes, sparse publication preservation and inactive-region reactivation.
Keep the ordinary correctness and GPU-assisted-validation passes separate from
Release timing. Do not create a new fixture or full-scene benchmark per subcase.

Evaluate 4096² first with spacing 4, then 1024²/2048². Record total lighting and
per-stage GPU time, actual completed rays/work, dirty coverage and maximum age,
edit-to-consumption latency, convergence behavior and complete live/retired memory.
Use unchanged Auto GPU power policy, freeze binary/shader/settings identities and
inspect images for stale illumination, leakage, missing fine structure and decay.
No unqualified timing extrapolation from spacing 16/32; no claim that scheduling
alone guarantees enough capacity at spacing 4.

After approval, amend the existing implementation plan in this order:
R8UI evaluation and spacing-4 admission; deadline/dependency scheduler and bounded
publication; variance/refresh selection and proven inactivity; consolidated
integration/performance checks and one final review. Retain only measured wins.
No commits or production edits are made during this design review.

## Reference inspiration

- Local prototype: `docs/oni_v22.slang`, `passMapSkipDist`, `Visibility`,
  `Voxel::DDA`, `Cascade::probeToUpdate` and `SuperSampling::update`.
- [NVIDIA DDGI update selection](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/main/samples/test-harness/src/graphics/DDGI_D3D12.cpp):
  variability with explicit invalidation; inspiration, not an interchangeable RC
  dependency or acceptance model.
- [NVIDIA probe variability implementation](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/main/rtxgi-sdk/shaders/ddgi/ProbeBlendingCS.hlsl).
- [Vulkan format capabilities](https://docs.vulkan.org/spec/latest/chapters/formats.html).

The selected architecture and priorities are user decisions. GPU-local bins,
cost-based adaptive quotas, complete-direction initial work items, conservative
inactivity and measurement-driven controller constants are proposed engineering
decisions in this document, not previously approved implementation details.

## Builder check

A fresh Sol rendering-engineering reviewer read this document and reported
“nothing important left”: no consequential unanswered question prevented planning.
This is a design-readiness check, not implementation or performance validation.
Design approval and explicit implementation resumption remain pending.

## Approval and resumption

The user requested implementation resume following this plan after the six-level
clarification. Treat that as approval of this design and inline execution. Keep
spacing4 and six configured levels (refinement plus five traced levels); no
map-size-driven increase. Implementation resumed2026-11-07. Earlier pending/paused
statements above record the review history, not current authorization.
