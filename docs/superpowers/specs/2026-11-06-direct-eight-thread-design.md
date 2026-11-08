# Eight-thread direct-window workgroups

Status: user-approved experiment completed; rejected for promotion on GPU cost. Original dispatch restored.

## Intent and evidence

Finish the existing world-lighting plan by testing its measured GPU parallelism
limit without changing transport values. The counter-enabled 4096² run measured
15.24 ms for a clean 512-receiver batch, 16.52% device SM activity, 1% device warp
occupancy, and 1.51%/0.24% DRAM read/write activity. Eight 64-thread groups expose
at most eight of 46 SMs. This supports a bounded scheduling experiment, not a
guarantee of speedup or proof that active SMs have no memory-latency stalls.

Evidence: `out/diagnostics/windowed-native-20261106/nsight-counters/report.md`.

## Approaches and selected scope

The user selected eight-thread workgroups over parallelizing ray directions.
Smaller groups preserve the existing per-receiver computation and require no
new radiance scratch storage. Direction parallelism could expose more work but
would require shared storage and a separately validated accumulation design.

Add an eight-thread entry point that calls the existing `worldRefine` body for
direct-window work. Keep the existing 64-thread entry points for diagnostic
straight/cascade and bounce paths. Select the matching pass-owned pipeline and
dispatch `ceil(receiver_count / 8)` groups only for direct-window batches.
Update the corresponding dispatch-limit admission to use eight threads.
Do not add a public workgroup-size setting or sweep configurations.

All eight lanes participate in the existing group barriers and initialize/flush
the eight shared statistics slots. Out-of-domain lanes still reach the barriers.
Integer query/segment/status totals must remain identical; the number of global
statistics updates increases with group count and is part of measured cost.

Preserve 256 directions, their order, all FP32 transport and CH arithmetic,
page masks, receiver quota, source identity, exact overlap copies, complete
fenced publication and error/teardown behavior. No CPU/GPU payload layout changes
are intended. Use the existing shader service and pass-owned pipeline lifecycle.

## Verification and decision

Extend the existing shared GPU fixture to compare the windowed eight-thread
path against the retained 64-thread straight control, including coefficients,
status and work totals. Retain the 1e-5 coefficient tolerance, physical 15%/1%
brightness/leakage gates, narrow partial groups, invalid admission and overlap
checks. Run ordinary then GPU-assisted validation, affected build and style
checks, and independent source review. Linux Vulkan is locally executable;
Windows compatibility remains an explicit unrun gate when no host is available.

After correctness passes, run the same Frontier4096 seed42, 512-receiver,
3840×2160 A/B witness and a counter-enabled capture if needed to establish SM
coverage. Freeze source/binary/settings and preserve raw outcomes. Restore the
temporary Editor quota selections afterward. Compare the full producer intervals,
request-to-present age and counters; do not equate high utilization with success.
The normal 6 ms, stress 10 ms, one-second age and 4 GiB lighting limits remain.

If the experiment fails, retain its evidence and return to diagnosis without a
workgroup/quota sweep or automatic promotion. A component pass advances the
remaining master-plan features and qualification; it does not complete the goal.

Builder details left open: the smallest existing pipeline storage extension,
test fixture placement, and counter-capture timing. These must not broaden the
selected paths or change the numerical contract.

## Result — 2026-11-06

The forwarding entry, matching pipeline/root selection and admission passed
ordinary GPU (42.27s), GPUAV (174.49s, zero Vulkan validation errors), asset
spatial (261.14s), formatting and independent source review. Physical L1 stayed
0.05785123966942149, blocked leakage zero, coefficient difference zero.

The same Frontier4096/512-receiver capture matched all40 baseline direct-work
records exactly. Warm producer peak11.702336ms, no-copy batches11.090048 and
11.110272ms: NO_GO against6ms normal and also above10ms. Matching pan present
617.651735ms passes only the age component. Median SM activity rose from17%
to96%/85%, but device warp occupancy stayed3.25%/2.49% and instruction issue
4.76%/3.59%; DRAM activity remained low. Broader SM coverage alone is insufficient.
One profiled pair is not a controlled speedup claim.

Candidate source is archived under
`out/diagnostics/windowed-native-20261106/eight-thread/rejected-source/`.
Original shader/pass/tests/codemaps and default256 Editor were restored exactly;
the three-target baseline rebuild passed9.822s. No sweep or promotion occurred.
The retained configurable-quota implementation and master goal remain unchanged.
See `out/diagnostics/windowed-native-20261106/eight-thread/runtime/report.md` for
counter definitions, context interference, raw trace and remaining gates.
