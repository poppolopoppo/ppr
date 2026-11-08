# Windowed direct-native world transport with temporal amortization

Date: 2026-11-05
Status: demand-window scope approved; configurable quota implemented; 512-receiver experiment rejected on GPU cost (2026-11-06)
Revision: 1

## Intent

The scalable cascade reconstruction has failed the unchanged 15% brightness gate in
every form tested: spacing-2 92.3295%, native spacing-1 56.8698%, 16-phase 65.3312%,
terminal-C1 55.2169%/33.6260%, transverse-plane 58.6777%, endpoint-primary 31.5083%,
and the v22 interval base 69.0801%. The one transport that passes is direct
native-cell tracing with 256 fixed directions and signed FP32 circular harmonics,
measured at 5.7851% L1 with zero closed-side leakage and zero matched-direction
traversal disagreement (`straight256_reference`).

That control is not scalable as-is: it traces the whole source. This design makes
the passing transport the production architecture by tracing only the active
receiver set, reusing probes across camera motion and unchanged revisions, and
amortizing refresh across frames so the 6 ms and 1 s gates become reachable. It
does not attempt another reconstruction.

## Why this is the right next step

- The gate is a spatial-visibility gate, not an energy gate. Endpoint-primary
  matched aggregate energy (0.2412 vs 0.2363) yet still failed at 31.5%, so
  numeric tuning cannot close it.
- The spacing-2 lattice is provably dead: at `x=7.5`, direct values
  `(0, 0.06640625, 0)` across `y=3.5/4.5/5.5` force at least `0.06640625` error
  under nonnegative bilinear DC, above the whole allowance
  `0.15 x 0.236328125 = 0.03544921875`.
- Direct native rays already pass and already store one 112-byte signed CH per
  native receiver in the windowed `direct_window` path. The missing piece is
  scheduling, not representation.
- 6 ms and 1 s are the product gates that cascade work never addressed.

## Goals

- Preserve the measured `straight256` result exactly on the 24-point fixture:
  open relative L1 <= 15%, closed leakage <= 1% of open, unchanged oracle.
- Meet <= 6 ms normal-play and <= 10 ms stress producer time on the active set.
- Meet <= 1 s request-to-matching-publication age for three cases: static camera,
  one door/emitter edit, and a camera pan to a cold window.
- Stay within the <= 4 GiB lighting storage budget, including overlap.
- Support the 4096^2 world extent while refreshing only the demand set.
- Keep the existing generation/fence/lease, cancellation and gameplay-query
  contracts.

## Non-goals

- No cascade, footprint connector, near/far interval, phase, probe-merge or
  bilinear-across-probes reconstruction work.
- No new global cache, singleton or implicit service.
- No visual matching, albedo, texture or lamp-density work.
- No change to the retained exact-path diagnostics or the secondary bounce
  component.
- No claimed Windows, live-VRAM, 4K60 or 6 ms result before measurement.

## Anti-goals

- Passing by clamping, omitting, blurring or mask tuning.
- A hidden latency model or any weakening of the 15%/1%/6 ms/1 s/4 GiB gates.
- Refreshing the whole 4096^2 world every second; that is physically impossible
  here and would be a fake gate.
- Using screen-space refinement to hide an offscreen door leak. The plan already
  states screen effects cannot repair a coarse-field leak through a closed door.

## Constraints from the current implementation

- `WorldRadianceTransport` is `{cascade_diagnostic=0, straight256_reference=1,
  direct_window=2}` (`App.Renderer.WorldRadiance.cppm`). `straight256` builds a
  full-map native CH field and traces 256 directions; `direct_window` plans
  32-cell pages, a 128^2 receiver window, copies overlapping donor generations
  and traces only missing strips (`App.Renderer.WorldRadiance.cpp`).
- `WorldRadianceCoefficients` is 112 bytes: 27 FP32 coefficients plus a status
  word. The existing windowed lamp run stored one row per receiver (2,304 rows,
  258,048 bytes).
- Generations are immutable and owner-confined, published behind fences and read
  through owned leases. Partial-initialization, cancellation, unknown-submit and
  teardown rules are already enforced.
- `WorldRadianceGameplayQuery` samples the same field; its windows already union
  with visible demand.
- Errors use `PPR_RETURN_*` macros; nothing is clamped. Time is injected.
- Storage accounting includes overlap and the caller's reserve.

## Approaches considered

- **A. Windowed direct-native transport, temporally amortized (chosen).** Keep the
  passing 256-direction native tracer; refresh only the demand window on a
  rotating schedule; reuse overlap; add screen detail later. Preserves the known
  quality pass and attacks the real cost/age gates.
- **B. Split transport.** Direct first-hit for the dominant term, coarse probes
  only for bounce. Plausible later, but introduces a second error budget and must
  first prove the bounce fraction; endpoint-primary shows aggregate energy can
  match while spatial error stays large.
- **C. Native-spacing probes with visibility-aware reconstruction.** The
  spacing-2 proof kills the cheap lattice, so at native spacing this collapses
  toward A's cost while still carrying merges. Rejected as a separate path.

A was selected because it is the only architecture with a demonstrated quality
pass and it converts the plan's temporal strategy into the mechanism that meets
6 ms/1 s.

## What already exists and what this adds

The windowed direct path exists today as `direct_window`: it plans 32-cell pages,
covers a 128^2 receiver window, stores one 112-byte signed CH per native receiver,
copies overlapping donor generations and dispatches only missing strips. The
straight-256 math and the signed-CH accumulation shader exist and pass.

This design adds only scheduling and evidence:

- a rotating per-frame probe/strip budget that fills or refreshes the active set
  over several frames instead of one burst;
- explicit freshness/age tracking per generation against the source identity;
- complete-generation staging so a partially refreshed field is never published;
- cold-pan and edit refresh driven by the same budget;
- structured cost/age instrumentation and the equality check against the control.

There is no new transport math, no shader math change and no new generation
format. The existing dispatch ranges are reused to select the strips/pages traced
in a frame.

## Design

### Field representation

- One probe per native cell centre in the active window.
- 256 fixed uniform midpoint directions, identical to `straight256_reference`.
- Signed FP32 order-4 CH, 27 coefficients plus status, 112 bytes per probe.
- Direct-first-hit transport: radiance and transmittance to the first opaque hit
  or world escape; sky closure at an exterior exit; no second bounce in this
  field. The existing separate secondary-bounce component is unchanged.

### Demand scope (decision requiring approval)

The supported world extent remains 4096^2. The *field* is demand-windowed: the
visible plane plus explicit gameplay windows, aligned to pages, exactly as the
current receiver planner does. The <= 1 s age gate is defined on the active
receiver set, not on every cell of the world. A whole-world per-second refresh is
not representable within 6 ms on this hardware; defining the gate on the demand
set matches the existing windowed architecture and the plan's "camera motion
reuses the world field" rule. This is a scope interpretation, not a gate
reduction, and needs explicit approval.

### Field values are unchanged

A probe's value depends only on its cell centre, the 256 directions and the
immutable source. Windowing, reuse and scheduling change *when* a probe is
traced, not *what* it contains. The 24-point comparison must therefore reproduce
`straight256` exactly, including the same bilinear evaluation used by the control.
If it does not, the change altered values and stops.

### Refresh and reuse

- A probe is fresh when traced against the current identity:
  `(world_generation, geometry_revision, content_fingerprint, source_revision,
  settings)`.
- Camera-only change: retain fresh probes in the overlap, trace newly exposed
  probes. This extends the existing `copy_donor` overlap to native CH probes.
- Source/world/geometry change: invalidate the affected probes. The conservative
  policy is to invalidate every probe whose 256-ray reach could intersect the
  changed cells (changed cells expanded by the far bound); a whole-active-set
  invalidation is the safe fallback if the region test is not exact.
- A rotating cursor refreshes a bounded probe budget per frame until every probe
  in the active set is fresh.

### Complete-generation publication (no partial field)

A generation is published only when every probe in its active set is fresh. While
a refresh is in staged progress, the previously published complete generation
stays bound and shading keeps reading it, so no frame ever samples a missing or
half-traced probe and there is no black flash or silent degradation. This keeps
the existing "publish only complete success" contract and adds no per-probe
validity flag to shading.

### Scheduling and age

- The per-frame probe budget is a configured setting, initially fixed from the
  measured ray rate and the 6 ms budget. There is no adaptive runtime controller
  in the first experiment; that would be a hidden latency model.
- Age = the frame the active set or its identity first differs from the bound
  generation, to the frame the next complete matching generation is published. A
  full refresh must complete in at most 60 frames (1 s at 60 Hz).
- Static camera: steady-state refresh keeps every probe within the age budget.
- Door/emitter edit: the old complete generation stays bound until the new
  complete, matching one publishes; age spans intent to that publication.
- Cold pan: newly exposed probes fill on the amortized budget; age is measured
  from the first frame that demanded the new window.
- If the configured budget cannot cover the active set within 60 frames, the
  experiment fails and reports it; there is no hidden fallback.

### Storage and overlap

- Active-window CH double buffer is small at 128^2 (about 1.8 MB per generation);
  parity and readback follow the existing accounting.
- A persistent whole-world CH cache (about 1.75 GiB at 4096^2) is explicitly out
  of scope for this design. Storage must be measured, not assumed.
- Two in-flight generations, source buffers, parity and the caller reserve are
  all charged before admission.

### Ownership, errors and teardown

- Keep the owner-confined immutable-generation model, fenced submission,
  cancellation drain and unknown-submit retention. No new global cache.
- Use `PPR_RETURN_*` macros; reject on failure; nothing is clamped.
- Teardown releases buffers in dependency order; the external `libdbus` LSan class
  stays a separately tracked unmet gate, not hidden by this change.

### Gameplay coupling

- The CPU gameplay query reads the same windowed CH. Its requested windows union
  with visible demand and participate in the same freshness rule. Stale or
  unmatched pairs remain rejected as today.

### Fine screen detail (stage 2)

Silhouette/contact refinement is a later bounded stage using reduced-resolution
depth, normal, coverage and material data. It consumes the world field as its far
input and never repairs an offscreen world leak. It is not part of this
experiment.

### What is preserved across a one-cell opening

There is no angular merge. Each native-cell probe traces its own rays with the
exact DDA; a one-cell opening is resolved because the ray origins are native cell
centres and the source occupancy is exact. This is why the analytic spacing-2
bound no longer applies. The residual approximation is the same native bilinear
CH evaluation that the passing control already uses; this design does not change
it.

## Feasibility model (estimates, not results)

- Measured: the windowed lamp field traced 589,824 rays in 8.221 ms, about
  71,700 rays/ms, on the fixed small scene.
- A 128^2 window with 256 directions is 4,194,304 rays, about 58.5 ms of tracing
  for a full cold refresh at that rate.
- Amortized over 32 frames: about 1.83 ms/frame of tracing, age about 0.53 s.
  Amortized over 10 frames: about 5.85 ms/frame, age about 0.17 s.
- These are estimates on a favorable scene. Real-world rays are longer and the
  experiment must measure both cost and age.

## The single pre-registered experiment

One ordinary registered run on the existing shared GPU fixture, then, only on a
pass, one GPUAV run:

1. Build the windowed direct-native field and let every probe reach freshness.
2. On the 24 points, require the unchanged 15% brightness gate and 1% blocked
   gate, and require the field values to match `straight256` within a stated
   exact tolerance.
3. Run steady-state amortized refresh and measure: producer ms per frame,
   rays/frame, request-to-matching-publication age for static camera, one
   door/emitter edit and a cold pan, and storage bytes.
4. Prediction that distinguishes this design: the 24-point L1 stays at the
   control's 5.7851% because the transport and evaluation are unchanged, while
   per-frame cost and age fall within 6 ms and 1 s.

If the quality check differs from the control, the window or scheduling changed
field values: stop, do not tune. If cost or age misses, report the measured
numbers; no sweep and no gate change. An actual-world run is a separate
checkpoint after this one passes.

## Risks and open questions

- Real-world traversal may be much slower than the small-scene rate.
- A cold pan may not meet 1 s if the per-frame budget is too small; the budget is
  set from measurement.
- Edit invalidation may be conservative and expensive; the region test needs care.
- Two in-flight windowed generations plus parity must be checked against 4 GiB.
- Teardown remains externally unclean on the world-radiance leaves.
- The demand-set age definition needs approval (see above).

## Decisions and my calls

- Chosen transport: windowed direct-native, no merge (your selection of A).
- Direct-first-hit only in this field; the secondary bounce stays separate.
- Keep 256 directions for this stage; reducing it is a later question.
- One experiment, one configuration derived from the measured rate.
- Demand-set age scope: proposed here, flagged for your approval.

## What is left to the builder

- Exact per-frame budget and frame-count target, derived from the measured rate.
- Exact edit-invalidation region and fallback policy.
- Whether to carry the inactive window cache or re-trace on revisit.
- Instrumentation placement and the storage-accounting line items.
- Test names, filters and the precise GPUAV policy after an ordinary pass.

## Approval

This design is not implemented until approved. The next step after approval is
the implementation plan, not code.

## Execution reconciliation — 2026-11-06

The user approved updating visible receivers plus explicit gameplay demands
within one second while preserving full-world source/occluder transport.
Current code already provides 32×8 receiver strips, one quantum per actual
rendered frame, exact immutable CH overlap copies and complete fenced
publication. These are preserved, not reimplemented. The active rectangle is
computed from demand; 128² is an example, not a fixed admission size.

The current CPU gameplay query traces a captured local optical hierarchy; it
does not sample the GPU CH field. Preserve that contract in this transport
increment. Broader gameplay parity remains a separate open gate.

Coordinator decision: unchanged immutable source/settings and covered demand
remain fresh; retracing identical static rays periodically produces no newer
information. Freshness age starts at a source/settings/demand change, using
explicit elapsed wall time, not a presumed 60 Hz clock. The 60-frame count is
a capacity check, never proof of the one-second gate.

First bounded candidate: configurable whole-row work quota, default 256 for
existing callers, experiment 512 receivers per quantum. The retained favorable
71,700 rays/ms estimate predicts approximately 1.83 ms for 512×256 rays, and
a cold 128² window needs 32 quanta instead of 64. This is a predeclared
experiment, not a world-cost prediction: the recorded real-world direct
quantum costs already warn it may fail. No automatic quota sweep follows a
failed measured cost/age gate. Preserve per-quantum u32 safety and u64 totals.

## Measured result — 2026-11-06

The bounded quota implementation passes the existing shared ordinary GPU leaf
(35.33s) and GPUAV twin (52.03s), with zero Vulkan validation errors, unchanged
5.7851% physical L1, zero blocked leakage and zero coefficient difference from
the straight256 control. Invalid quotas, narrow edge pages, quota-changed overlap
and copy-only publication pass. Default quota remains256; supported explicit
quotas are multiples of32 in[32,1024].

ONE actual Frontier4096 run used512 at the direct-only initial/pan settings.
Cold128² completed32 direct quanta; panB copied12288 rows and traced4096 in8
quanta. The first matching B present arrived in680.096ms, but frame-joined direct
and discovery peaked15.999968ms warm (5 frames over6ms), so the GPU gate is
**NO_GO**. Cold maximum12.423168ms is separate. The game saved3840×2160 pixels
and exited1 with an unsuppressed14131-byte/23-allocation LSan report. No VUID or
Validation Error text occurred. This is not clean teardown.

The512 runtime selection was removed; both Editor sites are byte-identical to
the pre-session implementation. Generic quota support/tests remain. No quota
sweep, default promotion or causal speedup claim follows. The admitted model
peaked1435863776bytes including a1073741824-byte estimated external reserve;
live VRAM remains unmeasured. The rejected all-pages model was7038543280bytes;
that stress dispatch did not execute.

Evidence: `out/diagnostics/windowed-native-20261106/world-direct512/summary.json`,
raw runtime log, source/binary freeze and frame-timing rows beside it. CPU
preparation/uploads, postprocessing/display GPU work, cadence/scanout, door/edit
age and a fully nonoverlapping cold pan are not qualified. This experiment
establishes correct configurable batching, not the full master plan. Further
transport architecture work is required before the world-scale cost gate can
pass; the current spec's stop-on-failure rule prohibits tuning this quota again.

## Hardware-counter follow-up — 2026-11-06

The user requested delegated Nsight Systems profiling and enabled per-user GPU
counter access. A clean copy-free batch measured15.24ms,16.52% device SM activity,
1% device warp occupancy and1.51%/0.24% DRAM read/write activity. The source's
eight workgroups on46SMs explain the observed activity ceiling. This does not
exclude memory latency/cache stalls within active SMs. Both profiling sessions
restored the original Editor and rebuilt default256 successfully.

The user selected a bounded eight-thread group experiment, preserving each
receiver's calculation; see [the short design](2026-11-06-direct-eight-thread-design.md).
The user approved that design and implementation. This is a new work-distribution experiment
supported by counters, not a restart of quota tuning. The original failed512
result and all product gates remain unchanged. Detailed evidence and capture:
`out/diagnostics/windowed-native-20261106/nsight-counters/report.md`.
