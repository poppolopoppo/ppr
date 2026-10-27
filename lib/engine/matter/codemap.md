# lib/engine/matter/

## Responsibility

`engine.matter` owns the direct pinned PB-MPM GPU solver and its resources. It
links core/math/shader/RHI, with no sim, physics, app or renderer dependency.
The game injects device/queue/shader lifetime and owns world/material/unit,
terrain, Box2D, edit and checkpoint adapters. Drawing never advances this solver.

## Public seams

- `Matter.Gpu.Types.cppm` (`:gpu.types`): source material enum, source default
  profile and explicit scalar CPU/GPU protocol. The semantic record `GpuParticle`
  is 88 bytes (CPU rows, snapshots, checkpoints, v5 wire); the device stores each
  particle as a 64-byte `ParticleWrite` hot row plus a 24-byte `ParticleReadonly`
  cold row, packed and unpacked by field name. Static collider stride32,
  parameter constant buffer64. F/D are four explicitly named source column
  components; no engine matrix storage inference.
- The contour/body extension has separate environment constants (32 bytes),
  immutable predicted end-pose body rows (64 bytes). `Matter.Gpu.Compact.cpp`
  losslessly encodes validated semantic terrain masks and canonical math tables;
  no physics/sim types enter. Eight masks share each u32, each contour case uses
  two u32 words (55 used bits), and all eight dyadic points share one u32.
  Geometry, case ordering and FP32 search arithmetic remain unchanged.
- `Matter.Gpu.cppm` / `.cpp` (`:gpu`): sole move-only `GpuSolver` owner. Devices with
  `WaveOps` select `fused0Wave`/`fused1Wave`; other devices retain `fused0`/`fused1`.
  These entries compile through injected `IShaderService` from one generic body
  with compile-time iteration and cooperation; `:gpu.sparse` owns maintenance
  pipelines, sparse maps, two payload pools, one budget pool and first-error resources. Every
  particle binding is the `g_hot`/`g_cold` row pair; the solver never sees the
  88-byte semantic record on the device.
- `prepareReplacement` freezes step admission and allocates every replacement
  particle/shape/readback/root resource before external transaction settlement.
  `publishReplacement` moves prepared ownership without allocation/GPU calls;
  cancellation retains committed state. Fresh roots prevent stale bound buffers
  from extending old allocation lifetimes. Accounting includes staged capacity.
- `prepareAppend` shares replacement admission/allocation but GPU-copies the
  current prefix and uploads only its new suffix. `pollReplacement` proves the
  submitted copy before the no-fail publication; `cancelPreparedReplacement`
  drains before releasing either source reader or candidate. Terrain-only edits
  upload no numerical rows. Numerical CPU-upload and GPU-copy bytes are separate
  accounting fields. The host uses this transaction for inventory edits.
- `submitResidentTick` copies committed to working and runs compact GPU discovery,
  count/allocate/insert and destination initialization plus two indirect fused
  dispatches per substep. K2 is the only admitted count;
  production uses eight evolving dt1/480 substeps. Iteration0 bootstraps scatter;
  iteration1 gathers and integrates. Each fused dispatch owns immutable selector
  and body-base constants through fence completion; its root belongs to the
  matching iteration-specific program. Fused0 omits unused numerical node preparation
  when a uniform finite-arithmetic certificate covers zero or one bounded box.
  Its coverage-only check uses a boundary-existence bit published with validated terrain;
  other collider/constants cases retain the original converged contact/error path.
  Fused1 retains full node preparation. Previous sparse A survives
  iteration0; current A is remapped only before iteration1. Destination pages and
  their one shared budget are initialized explicitly. Sparse capacity joins the
  prepared replacement transaction independently of spare Hot/Cold capacity.
  `pollTick` collects only the six-word error status.
- The caller validates dependent state before `publishTick`; discard preserves
  the committed epoch and rows. Fence/map/unmap errors remain copied on retry.
  Body inputs preserve collision geometry and wall velocity without reciprocal
  reaction reduction, buffers, CPU decoding or Box2D feedback.
- `beginSnapshot` / `pollSnapshot` own a full copy of one exact committed device
  epoch. Mutation stays frozen until take or drained cancellation. No automatic
  CPU mirror, candidate span or nonresident execution remains. Numerical fixtures
  publish validated results then inspect these explicit committed snapshots.
- `committedView` copies two strong row-buffer owners (hot and cold), the shared
  atomic epoch token, epoch and count in O(1). Publication revokes older
  unencoded values; shutdown revokes every value before draining. Callers expose
  the selected view only after their dependent CPU joint commit and use the same
  queue so encoded reads precede future buffer reuse. No renderer types enter
  matter.
- `drain` and `shutdown` are explicit lifetime boundaries; failed completion
  waits retain resources and dependencies must remain alive.

## Shaders and safeguards

Assets are `assets/shaders/pbmpm/{solver,fork_core,types}.slang`; source attribution,
accepted adaptations and the exact stage mapping are in its README/license.
All four source material branches, F/SVD/plasticity, static rotated boxes and
circles, tiny-mass threshold, post-contact re-encoding, volume feedback and
source integration/guardian safeguards are ported. Browser mouse/emitter/drain
controls are outside the admitted runtime.

Terrain replacement derives one boundary-presence bit per 8-by-8 dual-cell tile,
with 32 tiles per u32, alongside its validated packed masks. The table follows the same prepared,
published and cancelled resource ownership and peak accounting as those masks;
absent terrain uses the existing dummy binding. A 4097-by-4097 domain stores
8,392,708 mask bytes and 32,900 boundary-table bytes. These are allocation sizes,
not measured traffic or timing gains. Serial contour searches skip paired ring spans only
when both tiles lack segments, preserving nonempty-cell order, nearest ties and
the existing search termination and contact arithmetic.

Grid terrain queries converge every fused-group lane before wave cooperation.
Active lanes traverse one query's ring in parallel, select squared distance then
the original cell ordinal, and retain the prior ring's result on an equal
distance. Per-cell segment arithmetic and complete-ring termination are unchanged.
Endpoint queries retain the serial search. This adds no dispatch or device buffer;
the serial entries remain the capability fallback on devices without `WaveOps`.

Checked finite integer conversion and per-node/per-field uint CAS absolute-sum
budgets prevent signed scatter overflow; every cycle resets all four budgets.
Production reserves each rounded contribution in a shared absolute budget before
signed shared addition, then reserves each nonempty local total once in the same
cross-group sparse budget before its global signed merge. Signed cancellation
does not cancel absolute budget. Shared magnitude and contributor arrays add2048
bytes (4096 total per64-thread group); device resource accounting is unchanged.
Failure rejects the working candidate, with node/field/particle evidence. Tiny
encoded mass follows the selected fork threshold. No repair clamp is
added. Two sparse payload pools plus the four-field budget have explicit accounting. Byte accounting is
explicit. Ordinary completion collects only 24 status bytes. Demand snapshots separately
account full row readback; no measured speedup is claimed.

The four nonaccumulating shared-source node stores implement finite WGSL
f32-to-i32 conversion endpoints (-2147483648,2147483520), retaining nonfinite
rejection with actual node/field attribution. The planning-owner exception is
documented in the current recovery evidence and pins WGSL2026-10-21§15.7.6.
Scatter conversions and signed absolute-sum budgets still reject out-of-range
contributions; the node-only INT_MIN result never enters signed-negation budget
accounting. The temporary26-word stage probe is removed: ordinary completion is
again six words/24 bytes, with canonical reserved dispatch words restored.


Bucket addressing converts each already-validated nonnegative position to its
integer cell before unsigned division by6. This preserves the mathematical
partition required by `floor(position)-1` and its3-by-3 stencil; a floating
reciprocal near a six-cell boundary can select the next bucket prematurely.
Both production stencil traversals check their signed2D local coordinates
before flattening or shared access and reject a malformed candidate through the
existing structural-error sink. No valid contribution is clamped or omitted.
The pinned WGSL uses floating bucket division and assumes this support invariant;
this is a native discrete-address correction, not a PB-MPM equation change.
The failure probe proved local word index-16; exact failing substep position and
compiler lowering were not captured, so those remain unobserved.

## Tests and migration status

Phase1 sparse foundation is `:gpu.sparse` / `Matter.Gpu.Sparse.cppm/.cpp` with
`sparse_grid.slang` and shared `sparse_address.slang`. It owns checked-admission
lookup metadata, compact lists/groups/rows, two page payloads and one shared
budget pool; it only encodes into the caller's existing encoder and never submits.
Every dispatch has a fresh retained immutable root. Separate publication/consume
passes and UAV barriers require no GPU spin. A current-list slot is identical
for both destinations and their budget. `settleEncoding` requires caller fence
proof or definite unsubmitted abandonment; unknown recording prevents reuse.
The argument buffer is bound as a UAV only to its finalizer writer. Existing RHI
state tracking transitions clears, shader writes and indirect reads at their
actual uses; manual argument-state round trips are omitted. Global ordering
barriers remain between maintenance/fused dispatches.
The coordinator-released Phase2 production switch passed independent review,
touched-file format/style, the Dev build and 22 focused GPU/oracle leaves on
2026-10-23; the full integration and product acceptance gates remain open.
Evidence is in `out/diagnostics/pbmpm-recovery-20261022/port/sparse-phase2-20261023/`.
Dense production payloads/budgets and world-sized
bucket scans are removed; the independent dense fork oracle remains test-only.
Production pairs recording with definite unsubmitted settlement after abandoning
its encoder, or executed settlement after the matching fence. Ambiguous submission
or completion retains the open recording and owners. Mandatory GPU groups label
tick/substep/discovery/initialization/fused/status and particle draw work. Optional
timing now reserves 44 endpoints for eight substeps and reports eight stages;
these are GPU/API elapsed diagnostics, not CPU execution-cost measurements.

`engine.tests.app` registers `app/matter_gpu` with explicit D3D12/core and GPU
validation, runtime compilation/reflection/pipeline/readback, transfer overflow,
collection retry, split-row pack/unpack, replacement accounting and the D4
dual-owner peak-footprint probe. It needs no window/demo.
`source_checks.slang` exercises actual port helpers, not a second solver.

The retired CPU `.Numerics`/`.Reference` partitions and sim `Matter.Tests.cpp`
are gone from the tree; the original source and halted CPU fidelity edits remain
archived in the task's `.slim` evidence only. They are not a fallback backend.

Current numerical, resident-status, publication-validator and append fixtures
use the same resident execution path. The completion coverage/retirement ledger
is in `out/diagnostics/gpu-authoritative-20261020/completion-20261021/`;
current integrated execution remains a separate validation gate.
