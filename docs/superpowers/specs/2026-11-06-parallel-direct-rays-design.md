# Parallel direct rays with ordered coefficient accumulation

Status: user selected implementation on 2026-11-06 after evaluation of walker,
parallelism and scheduling leads. This records that selected bounded approach.

## Intent and approaches

Finish the existing master plan without weakening6ms normal/10ms stress GPU,
1s demand freshness,4GiB lighting or15%/1% quality gates. The eight-thread
experiment exposed more SMs but still took11.70ms; useful warp occupancy and
instruction issue remained low. The user selected parallel rays first over a
precomputed vacuum-skip lookup. Adaptive budgets/variance remain a later scope.

## Selected design

Only direct-window refinement uses a new64-thread workgroup-per-receiver entry.
GroupID.x selects the receiver; GroupIndex selects directions lane+64*k for
k=0..3. Each included direction calls unchanged `worldTraceRaw`; omitted page-mask
directions explicitly write zero radiance and zero status. Four group-shared
arrays hold256 R/G/B/status values (4096 logical bytes per group). Including
the existing eight integer counters, total declared shared storage is4128bytes;
actual backend allocation and residency remain to be measured. Separate arrays
keep adjacent lane writes adjacent within each component. All lanes reach the
group barrier before lane0 consumes the results in phi0..255 order.

The existing projection expressions, constant-field detection, error/status
propagation and normalization execute in their original order in lane0. No
floating-point wave sum or tree reduction is introduced. Small shared projection
helpers may remove duplication with the retained reference path, but must not
reassociate expressions, alter casts, direction generation or numerical guards.

Use SV_GroupIndex for ray indexing; do not confuse wave-local lane IDs with the
64-thread group index. Wave intrinsics are not required for this first candidate:
the existing eight-slot group counter reduction preserves exact integer totals.
Every lane contributes its trace work, but only lane0 reports one output row and
its aggregate status. Inactive groups/lanes must still obey uniform barriers.

Pass-owned program/pipeline/root selection and dispatch admission match one
group per receiver. Legacy straight/cascade and bounce paths retain their64-thread
entries. Quota, masks, immutable sources, overlap copies, complete fenced
publication, cancellation and resource teardown remain unchanged. No uploaded
payload ABI, public tuning setting or full-world ray scratch allocation is added.

## Acceptance and decision

Test new entry discovery/group dimensions, direct-window/reference coefficients
(1e-5), status and query/segment/skip/row counts in the existing shared fixture.
Include nonblack sky, mixed medium/emission, black-sky masking, narrow receiver
windows, constant fields, unchanged wall/aperture gates, overlap and invalid
admission. Tests must detect missing/duplicated direction lanes and masked-bin
stale shared memory. Ordinary then GPUAV, affected builds/style and independent
source review precede the same Frontier4096 seed42/512-row/4K bounded witness.

Record actual GPU costs, age, clocks, SM/warp/issue metrics and sample limitations.
Keep sanitizer/runtime settings matched for performance comparison; lifecycle
validation uses the existing documented policy separately. If the candidate
meets component gates, obtain unprofiled confirmation before promotion. If it
fails, archive evidence and restore the original path; no automatic sweep.
Success here does not close integrated matter/feedback/media/gameplay, total
postprocessing/cadence/memory/teardown or other master-plan acceptance gaps.

## Deferred skip lookup note

The user identified Vulkan R8UI as preferable to V22's WebGPU-oriented R32UI.
An8-bit encoded hierarchy class covers all13 levels at4096² and costs16MiB per
full-world image rather than64MiB R32. Treat this as preferred design input for
the later walker experiment, subject to actual format usage/device/Slang binding
verification and generation-overlap/build costs. It is not part of this change.
