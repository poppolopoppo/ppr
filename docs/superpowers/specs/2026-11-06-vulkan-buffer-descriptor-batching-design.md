# Batch Vulkan buffer-descriptor writes

Status: approved by the user on 2026-11-06. Establish an optimized baseline and measure validation separately. NRI/NVRHI migration is out of scope. No measured batching benefit yet.

## Intent and evidence

Reduce CPU preparation delay that prevents the full-world lighting demand from
becoming ready within one second, without changing lighting, simulation work or
validation policy. Three matched Debug/sanitizer observations measured median
sparse dispatch recording at 0.260845 ms with RHI/native validation, 0.258240 ms
with native validation alone, and 0.020240 ms with neither. Native validation is
the dominant measured contributor to this phase. These are single sequential
observations, not exclusive call-stack attribution or release qualification.

Pinned Slang-RHI currently calls vkUpdateDescriptorSets once per descriptor.
Batching is a hypothesis about reducing repeated native-layer entry overhead;
validation of individual descriptors will still occur. No speedup is promised.

## Approaches

1. **Recommended: batch consecutive raw-buffer descriptor writes locally.**
   Keep all checks and descriptor values, but submit an ordered array in fewer
   API calls. Small Vulkan-only change; requires a maintained private overlay.
2. Cache/reuse descriptor bindings or shader roots across dispatches. Potentially
   avoids more work, but changes mutable constants, in-flight ownership and
   invalidation; defer because the present evidence does not justify that scope.
3. Keep the backend unchanged and qualify a separate performance configuration.
   Useful for normal-play performance evidence, but does not improve the
   existing validation-enabled freshness failure. Do not relabel it as a pass.

The user approved approach 1 and the bounded experiment below, with representative
optimized performance measured separately from validation. NRI/NVRHI are out of scope.

## Smallest implementation

Use a hash-checked generated private copy of src/vulkan/vk-shader-object.cpp,
following the existing CMake surface-overlay mechanism. Leave the shared CPM
cache and upstream pin unchanged. Reviewed source SHA-256:
99baec5d1c7284b531fd0119093c38a621bed5fd698400b90d81304beaee53b3.

Within BindingDataBuilder::bindAsValue, collect consecutive RawBuffer and
MutableRawBuffer writes into a local fixed-capacity batch of 32 writes. Capacity
is a scratch bound, not a workload cap: flush when full and continue. Preserve
one write per descriptor, original set/binding/array index/type, buffer handle,
range and null-buffer values. Preserve resource-state tracking in its existing
order. Ordinary constant-buffer descriptors and all other descriptor types retain
the existing immediate path.

The batch owns both VkWriteDescriptorSet entries and their VkDescriptorBufferInfo
payloads. It must not retain pointers to stack locals from a helper, move while
pointers refer to its storage, allocate per descriptor, use global/TLS state, or
cache resources across calls. Verify actual size/alignment and bounded stack
cost rather than assuming a type layout. Flush before every non-raw-buffer
binding range and before recursive sub-object processing. Flush pending writes
on early return as required to preserve the existing issued-write order; do not
leave writes pending after the binding call. No descriptor-set reuse or updates
to already submitted sets are introduced.

Vulkan specifies that writes in pDescriptorWrites execute in array order:
https://docs.vulkan.org/refpages/latest/refpages/source/vkUpdateDescriptorSets.html
The existing synchronization and descriptor validity requirements still apply.

Measure baseline and candidate in the same optimized Release configuration with
validation disabled, then observe core validation separately. Retain both RHI
debug wrapping and native core validation for correctness checks, with the
registered sanitizer policy. Keep the 512-receiver quota, rays, summation order,
simulation substeps, dispatch count, barriers, frame admission, fences and GPU Auto policy.
No shader, public RHI API or GPU resource-layout changes.

## Verification and decision

- Verify generated-source hash guards, registration, idempotent regeneration,
  unchanged CPM source and Vulkan-only scope; document the private patch.
- Exercise empty, single, exact-capacity, overflow and mixed-type boundaries,
  confirming each descriptor's payload and order remain valid at submission.
  Include early exit/nested binding lifetime coverage. Reuse existing native
  Matter and rendering fixtures for actual GPU output and resource lifetime.
- Build the affected target closure, run relevant ordinary and GPU-assisted
  correctness checks, and obtain an independent source/lifetime review.
- Repeat the existing 4096-world/4K/128-cell pan in matched optimized baseline
  and candidate builds, without temporary CPU subphase probes. Measure validation
  separately; retain sanitizer coverage in correctness runs. Verify native update
  call counts and unchanged workload, and report readiness, GPU component timing
  and clean shutdown separately. Distinguish profiled API time from frame latency.
- Retain a production patch only if correctness and measured cost evidence support
  it. A smaller CPU time alone does not close the one-second requirement, total
  GPU frame, live VRAM, quality, full-world mutation or remaining plan gates.
  If no material gain is measured, remove only this experiment's changes and
  record the result without weakening validation or trying unrelated changes.

The implementation plan follows approval. Existing diagnostic source edits have
already been restored and rebuilt; this document does not authorize treating
those diagnostic bypasses as production changes.


## Experiment decision

Implemented and evaluated on Linux/RTX2080 in optimized baseline/candidate
configurations, with validation measured separately. Native update calls fell
62.5%, but recorded API savings totaled only4.632ms over the capture and no
end-to-end improvement was demonstrated. Independent review recommends withdrawal
under the material-gain criterion above. The coordinator accepts that decision:
remove the production overlay and preserve the experiment evidence, without an
RHI migration or weakened performance/correctness gates. See
`out/diagnostics/windowed-native-20261106/descriptor-batching/report.md`.
