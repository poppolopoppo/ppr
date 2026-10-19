# Unified Matter Simulation v2 — fixed-resolution PB-MPM, gas field, Box2D coupling

Oct 18, 2026 · @poppolopoppo · revised after the bandwidth and stability review

## Summary and scope

Keep fixed-resolution PB-MPM for moving liquids and granular matter, Eulerian cells for gas, and Box2D for rigid bodies. Drop v1's adaptive particle split/merge. The initial implementation is deliberately smaller than the earlier v2 draft: one liquid first, terrain collision, conservative sleeping, one cellular gas representation, and analytic fluid forces on bodies. Liquid stratification remains a required behavior, with an early feasibility experiment before committing to its coupling algorithm.

This is a design/proposal record, not an implementation inventory. It supersedes the matter sections of [v1](../physics/unified_particle_simulation.md) only for tasks adopting this plan. The [documentation guide](../README.md), [engineering contract](../../AGENTS.md), current source and CMake retain their authority. No timings or target capacities in this document are measured.

**Evidence labels:** ESTABLISHED describes a published method or an inspected implementation, not this combination of features. PROPOSED describes PPR's implementation choice or an unvalidated extension. ARITHMETIC describes a calculation with explicit assumptions. TARGET-PENDING means hardware acceptance has not run.

| Area | Initial decision | Deferred or removed |
| --- | --- | --- |
| Matter | Fixed dx = 1/3 m; N = 4 equal-reference-volume samples per cell; PB-MPM | Adaptive split/merge; replacing PB-MPM without a new design decision |
| Gas | One species per cell; integer mass; isothermal conservative pair exchanges | Building several mixture rungs; entrained bubbles; acoustic or general compressible-flow accuracy |
| Sleeping | Remove whole quiescent interacting regions from solver work while retaining their state | Partial-reservoir freezing without a support model; automatic quantization of constitutive history |
| Storage | Separate iteration-hot state, read-mostly mechanical metadata, cold state and visual output | One universal 20-byte particle contract; unvalidated FP16 state |
| Buoyancy | Early oil/water experiment; analytic Archimedes and drag for bodies | Claim that a shared velocity field forbids all buoyancy; unproven drift invariant |
| Gas–matter coupling | Cell occupancy, deterministic displacement and an exact displaced-mass ledger | Singular 1/free-volume forces; trapped-gas pressure feedback and diving-bell promises |
| Scheduling | One fixed rate and iteration profile per interacting matter domain | Per-brick tick-rate LOD; changing stiffness by hardware without behavioral acceptance |
| Hardware | Analytical accounting and RTX 2080 experiments now | Claiming Deck/One S acceptance from repeated desktop workloads |

“Unified” means shared material identity, geometry, conserved-quantity accounting and explicit coupling interfaces. It does not mean identical storage or a single solver for all phases. Finite output is necessary but not sufficient: volume preservation, rest stability, contact response and phase separation are separate gates.

## Instructions for coding agents

- Apply only the adopted phase. Preserve the existing decision-log rows; append decisions and explicitly identify what they supersede.
- Do not implement adaptive split/merge, gas as PB-MPM particles, SPH/PBF, FLIP, a pressure Poisson solver, or a dense full-world simulation grid in this scope.
- Implement each feature with its behavior scenes, resource bounds and diagnostic counters. Do not advance past a failed required behavior gate. TARGET-PENDING does not block development phases.
- Establish a simple FP32 reference first. Compare optimized kernels against that reference before replacing it; keep the reference available in the validation harness.
- Report logical access bytes, allocated/resident bytes, measured GPU time and hardware memory counters as different quantities. Do not call source-level byte counts measured DRAM traffic.
- Treat workgroup size, wave specialization, precision, iteration count and timestep as measured choices, not universal speedups. Change one optimization at a time and retain a compatible baseline.
- Reuse engine allocation, jobs, RHI/shader, completion and error facilities. Keep simulation ownership outside the content-free Renderer. Follow [Slang guidance](../../.opencode/skills/slang-shader-developer/SKILL.md) for layouts, bindings and target validation.
- Every capacity limit needs an explicit failure/result path. No particle or gas mass may be silently deleted, clipped, overwritten, or frozen because the active budget was exceeded.
- Preserve user work. An unexpected compiler/feasibility blocker is returned with evidence under the repository's delegated-plan procedure, not worked around by changing the selected solver.
- Use the [validation](../../.opencode/skills/validation/SKILL.md) and [build-system](../../.opencode/skills/build-system/SKILL.md) procedures for implementation. Documentation changes alone do not establish runtime or performance acceptance.

## Repository foundations and ownership

The following are integration inputs from the existing plans and previous source inspection. Recheck the owning maps and current source when implementing; no new module or seam below is asserted to exist.

| Input | Reuse |
| --- | --- |
| [Static-world plan](static-world-generation-implementation-prompt.md) and [simulation map](../../lib/engine/sim/codemap.md) | 4096 × 4096 cells, h = 1/3 m, 128 × 128 chunks, initial matter spans, shared marching-squares contour |
| [Physics map](../../lib/engine/physics/codemap.md) | Box2D boundary, terrain chains, body handles/poses and existing coupling declarations |
| [Game map](../../game/codemap.md) | Driver-owned content, world jobs and coordination of sibling simulation/physics systems |
| [Frame map](../../lib/engine/app/frame/codemap.md) | Owned submissions, completion-governed slots, cancellation and teardown |
| [Shader map](../../assets/shaders/codemap.md) | Runtime Slang asset and staging conventions |
| `docs/oni_v26.slang` prototype | A compact visual/map cell encoding and catalog vocabulary; not evidence of a PB-MPM solver |

Proposed `engine.matter` depends on core, math and RHI/shader, never on sim or physics. The game driver supplies numeric material tables, terrain geometry/deltas, initial matter, body descriptions and commands. Renderer encodes submitted work without owning matter state. Add partitions only as their phase needs them: catalog, particles/grid, scheduling, terrain, gas, colliders/coupling and diagnostics. A new source and its CMake registration remain one implementation change.

Use the existing application clock and fixed-tick identity. Compute submission must execute every accepted simulation tick even if no surface is visible; a render callback cannot become the simulation clock or silently skip ticks. Verify the compute submission seam before choosing its integration point. Lower-level kernels consume explicit dt and iteration indices.

## Performance contract and hardware evidence

### Targets and limitations of the development machine

| Device | Public facts | Interpretation |
| --- | --- | --- |
| Steam Deck LCD | 8 RDNA 2 CUs, 1.6 TFLOPS FP32; LPDDR5-5500 across 128 bits, hence 88 GB/s theoretical | CPU and GPU share memory and APU power; use the LCD configuration as the initial Deck floor |
| Xbox One S | 12 CUs, 1.4 TFLOPS; 68 GB/s DDR3 plus a separate 32 MB ESRAM at 218 GB/s | ESRAM bandwidth is not additive general-purpose DRAM bandwidth; placement and console APIs are separate work |
| Development PC | Ryzen 9 3950X, RTX 2080 with 448 GB/s, 64 GB system RAM | Useful for correctness, access-pattern investigation and relative A/B tests; not a target emulator |

Sources: [Valve LCD specifications](https://www.steamdeck.com/en/tech/deck), [Microsoft family specification](https://news.xbox.com/wp-content/uploads/sites/2/Xbox_One_Spec_Sheet.pdf), [NVIDIA RTX 2080 specifications](https://www.nvidia.com/content/nvidiaGDC/zz/en_ZZ/geforce/graphics-cards/rtx-2080.html). Deck OLED is a different configuration. Public [Slang-RHI backends](https://github.com/shader-slang/slang-rhi) do not by themselves establish an Xbox console deployment path; platform/compiler access and the console integration remain TARGET-PENDING.

Running six or seven independent copies on the RTX is an optional stress experiment, not hardware emulation or an acceptance gate. Another arbitrary safety multiplier cannot reproduce cache hierarchy, atomics, occupancy, wave width, driver costs or shared power. CPU affinity on a 3950X likewise does not reproduce Jaguar. No requirement to obtain a Deck before M2 remains.

### What must be accounted for

Maintain a per-pass ledger with: invocation count per tick, active particles P, owned nodes G, tiles T, gas cells C, input/output fields and byte widths, logical reads/writes, atomic operations, transient/resident allocation, dispatch/barrier count and timestamp. Include setup, initial/final transfers, clears, bucketing, allocation/compaction, wake/sleep, SDF edits, bodies, diagnostics, checkpoint staging and the visual bridge.

Use the actual schedule:

```text
B_logical_tick =
    B_commands + B_activation + B_bucketing
  + B_initial_transfer
  + sum(iteration passes, including grids and clears)
  + B_final_integration + B_sleep
  + sum(gas exchange phases)
  + B_geometry + B_body_sampling + B_reductions + B_visual_bridge
```

Track logical traffic separately from DRAM/cache traffic. Reused cache lines can lower DRAM demand; uncoalesced accesses, atomic transactions and spills can increase cost. For each serial pass, time is bounded below by its limiting resource (memory traffic/effective bandwidth, arithmetic/effective throughput, or atomic service rate); dispatch, barriers and dependencies add further costs. Do not derive a frame prediction from bytes alone or add mutually overlapping resource bounds as if they were independent work.

P alone is insufficient. Report G/P, tiles/P, particles per tile distribution, halo duplication, maximum bucket occupancy, material-model mix and body overlap. A dense tank and fragmented droplets with the same P need different grid and scheduling work. Clear and step allocated active pages, not the maximum particle capacity or the whole world.

### Arithmetic envelopes, not capacity promises

The old 65% bandwidth derating, one-third simulation share and 70/20/10 subsystem split are retained only as sensitivity assumptions. They are not measured utilization or approved product budgets. At 68.3 GB/s × 0.65, the assumed One S bandwidth is 44.4 GB/s. One third over a 60 Hz frame is 247 MB and already represents 5.6 ms of bandwidth service at that assumed rate, before other bottlenecks.

The earlier 850 B and 135 B per particle per tick and the resulting 0.2–1.6 M particle caps are withdrawn as capacity estimates. For example, reading and writing an entire 20-byte state three times is 120 logical bytes before any grid, bucketing or diagnostic work. Field specialization may reduce that; separate kernels may increase it. Only a concrete schedule can establish the model.

| Awake particles | At 200 B/particle/tick | At 400 B/particle/tick | At 800 B/particle/tick |
| --- | --- | --- | --- |
| 100 k | 20 MB; 1.2 GB/s | 40 MB; 2.4 GB/s | 80 MB; 4.8 GB/s |
| 200 k | 40 MB; 2.4 GB/s | 80 MB; 4.8 GB/s | 160 MB; 9.6 GB/s |
| 500 k | 100 MB; 6 GB/s | 200 MB; 12 GB/s | 400 MB; 24 GB/s |
| 1 M | 200 MB; 12 GB/s | 400 MB; 24 GB/s | 800 MB; 48 GB/s |

ARITHMETIC: decimal MB/GB, 60 Hz, hypothetical complete matter costs; gas, rendering and GI are additional. At N = 4 and h = 1/3 m, one square metre at full packing has 36 particles; a 1,000 m² pool has 36 k. Begin scale sweeps in tens to low hundreds of thousands, then extend until the limiting resource is visible. These are experiment sizes, not supported capacities.

The old gas estimate, (1.56 reads + 1 write) × record size, describes one 8 × 8 tile stencil with a one-cell halo. It does not price a complete conservative transport algorithm. The selected four-phase pair exchange below costs approximately 4 × (8 B read + 8 B write) = 64 logical B per stepped cell when all pairs participate, before geometry, activity and probes. At 5 M cells and 60 Hz that alone is 19.2 GB/s; a proven 4-byte cell candidate halves those state bytes. Count the real active phase set.

Memory capacity also needs a budget. Fully expanding 16.8 M cells into N = 4 particles at 40 B would require 2.5 GiB for those fields alone; a 24 B candidate still needs 1.5 GiB. Particle/grid scratch, gas, terrain, staging and rendering are extra. Initial slot bricks and sleeping pages must not all be expanded on world load.

### Acceptance without target hardware

- **Design acceptance:** complete access/allocation model and numerical range analysis, with uncertainty and worst-case scenes.
- **Development acceptance:** CPU-reference comparisons, replay checks and RTX per-pass A/B measurements; no target-speed claim.
- **Platform acceptance:** sustained integrated-game runs on Deck and One S with rendering/GI/CPU load, representative long sessions and wake/edit bursts. Record backend, compiler, driver, clocks/power mode, dt, iterations, scene identity, timing distribution and memory counters where available.
- Platform acceptance remains TARGET-PENDING until performed. M0–M7 engineering work may proceed on passed development gates. Do not certify a target cap or freeze target-facing quality promises without that evidence.
- M0 records proposed simulation CPU/GPU frame-time and memory allocations alongside the game/GI budgets; the task owner must adopt concrete product values before declaring performance acceptance. This plan invents no engine-wide threshold.

## Numerical and conservation contract

Use metres, seconds and kilograms, +Y up, and the existing row-vector matrix convention. The 1 m reference thickness remains the working assumption: cell volume = 1/9 m³ and particle reference volume = 1/36 m³. Particle mass is reference density × reference volume; current volume can change under the material model. N and dx do not change by hardware tier.

1. **Material ledger:** exact integer particle/slot counts per material and exact gas quanta, including displaced/overflow records. Compute diagnostic masses from those quantities; floating-point P2G sums are not the authoritative mass ledger. Future phase changes need a defined cross-representation mass quantum/remainder policy.
2. **Finite state:** validate persistent and scratch outputs; invalid state or overflow fails the affected tick with a diagnostic, not deletion.
3. **Complete ticks:** commit mutations and publish results only for a complete tick. Preflight page, particle, command and readback capacity. A GPU-detected failure leaves a recoverable committed state via versioned source/destination or equivalent bounded transaction storage; include its cost.
4. **Geometry:** consistent terrain contour and face connectivity; no accepted penetration beyond the tested tolerance. Projection is a recovery mechanism, not a proof of continuous collision.
5. **Coupling:** uniquely owned samples, tick-stamped results, generation-checked bodies, at-most-once application, explicit stale/missing-result policy.
6. **Rest and volume:** bounded hydrostatic compression, surface drift and energy/centre-of-mass error at the adopted dt and iteration count. Finite output alone does not pass.
7. **Reproducibility:** compare canonical state by stable identity on the same compiler/GPU/driver. Integer addition helps only if range is valid and inputs, allocations, quantization, ordering-sensitive operations and control flow are also repeatable.

Changing iteration count or dt is a behavioral change. Start comparisons at 60 Hz and 2, 3 and 5 iterations, then adopt one validated profile for interacting matter. Retuning relaxation does not guarantee equivalent convergence at different depths. A uniform 30 Hz profile is a separate candidate, not an automatic half-cost win; the old 0.5 dx/dt movement limit would become 5 m/s instead of 10 m/s.

## Storage, locality and precision

### Access-frequency separation

“Cold” describes access frequency, not necessarily CPU residency. Data needed every iteration stays GPU-local. Keep an authoritative source for each value; do not duplicate entire records for convenience.

| Storage group | Contents and cadence | Initial representation / opportunity |
| --- | --- | --- |
| Iteration-hot liquid state | Candidate displacement d, affine displacement D, tracked liquid-volume state J | FP32 2 + 4 + 1 scalars = 28 B; avoid spilling intermediate results between fused operations |
| Read-mostly particle inputs | Position for stencil weights; material/model lookup | FP32 local position 8 B + metadata word 4 B; read-hot but mostly write-cold during iterations |
| Model-specific hot state | Deformation F, plasticity history for granular/soft models | Separate sidecars only for models using them; no universal scalar substitute for F |
| Scheduling metadata | Stable identity/generation, page/tile membership, ranges, free lists, neighbor pages | Separate integer arrays, accessed by activation/bucketing/reorder; indirection bytes count even when the solver record is compact |
| Mechanical catalog | Density/reference mass, relaxation, friction and precomputed constants | Small read-only table; separate liquid/granular parameter subsets where useful |
| Cold catalog | Names, authoring data, color, reaction descriptions and unused thermal data | CPU/content ownership or distinct GPU tables; never embedded in every particle |
| Sleeping state | Positions, model state and the adopted motion state of dormant regions | Retain complete state initially; no per-iteration scan or GPU/CPU round trip |
| Gas mechanics | Species and integer mass, activity and face connectivity | Isothermal 8 B cell baseline; pressure derived, temperature uniform until thermal work |
| Geometry | Terrain narrow-band SDF/apertures, body tile lists | Persistent, dirty-region updates; no full rebuild per tick |
| Rendering/GI | Quantized dominant material, visual mass/coverage and temperature when available | Separate 4 B candidate plane; explicitly lossy and never authoritative for simulation |
| Checkpoint/diagnostics | Snapshot staging, ledgers and sampled probes | Versioned staging and small readback summaries; full-pool audit on validation/save, not every shipping tick |

A liquid FP32 baseline comprises 28 + 8 + 4 = **40 B of fields**, excluding stable IDs, indices, allocation metadata, model sidecars and transaction copies. A velocity/displacement buffer may be reused across timestep phases: initialize d from v and forces once, iterate d, then recover v = d/dt at commit. Name phase semantics explicitly; never apply gravity once per solver iteration.

Begin with coalesced SoA streams. Compare small AoSoA blocks if the fused kernel consumes most fields together; SoA is not automatically superior for every access pattern. Use explicit integer words for packed storage, and verify target reflection, offsets and stride rather than assuming C++/Slang padding.

### Packing candidates and their gates

| Field | Candidate | Benefit | Required evidence / rejection condition |
| --- | --- | --- | --- |
| Position | Two u16 offsets relative to an 8h tile | 8 → 4 B | Quantization step ≈ 40.7 µm, nearest rounding error ≤ 20.4 µm per axis; test slow creep, long-term rest and tile crossings. Sub-quantum motion can disappear. Keep FP32 if this changes behavior; a residual buffer has a cost |
| Velocity or candidate displacement | half2 storage, FP32 arithmetic | 8 → 4 B | Prove range and error at adopted dt, include fast impacts and very slow settling. Encode/decode once per necessary storage boundary |
| Affine displacement D | Four halves, FP32 arithmetic | 16 → 8 B | Shear/rotation, free fall, density drift and contact regression; no NaN/Inf or excessive accumulated dissipation |
| Liquid J | Keep FP32 initially | Avoid lost small volume corrections | Half spacing near 1 is about 0.001; do not assume that repeated small corrections survive half rounding |
| Material/flags | u16 material plus u16 flags in one word; optional u8 palette later | Stable 4 B baseline; shared metadata reads | Prove catalog/palette range and reserved IDs. Packing subfields must not create competing writers to one word |
| F/plasticity | Model-specific FP32 initially | Liquids do not pay for unused tensors | Constitutive tests before reducing precision; sleeping cannot reset plastic history |
| Gas | u32 quanta + one metadata word initially; optional 24-bit mass + 8-bit species in one word | 8 → 4 B | Fixed quantum, species cap, max mass and source overflow are explicit. Never use logarithmic/FP16 mass as the conserved ledger |
| Temperature | Uniform during isothermal phases; later separate fixed-point or FP32 thermal plane | No repeated unused temperature traffic | Thermal/phase-change work must define range, energy/remainder accounting and cold/hot transitions |
| SDF / face aperture | Narrow-band FP16 SDF; normalized aperture candidate | Geometry bandwidth | Compare collision and open-face topology against FP32 contour geometry; quantization must not create or seal a passage |
| Visual plane | Prototype-style packed display fields | GI need not read physics state | Saturation is visual-only and diagnosable; never feed clamped display mass back into physics |

The first combined liquid candidate is **24 B**: position 4, velocity/displacement 4, D 8, J 4 and metadata 4. It is conditional on the individual gates, excludes sidecars and scheduling data, and is not a promise of 24 B per iteration. The earlier universal 20 B claim is withdrawn.

Tile-relative storage needs an origin available to every consumer, including rendering, save/readback and migration. A tile may own multiple work chunks. Rebucket/rebase after final integration, with a checked transition representation for particles crossing tile bounds; never clamp a crossing position into its old u16 range. Price scratch and origin/index reads.

Do not re-encode values more often merely because kernels are fused. Hoist invariants and keep FP32 intermediates in registers where occupancy permits. FP16 storage can save traffic even where FP16 arithmetic has no throughput advantage. Do not assume Xbox One S has later GCN/RDNA packed-math features.

### Integer accumulation and deterministic reductions

Use 32-bit integer atomics as the portable initial P2G candidate, with separate scales for mass, displacement-weighted momentum and volume when their ranges differ. Derive bounds from maximum material density, local particle concentration, displacement and affine contribution D × offset, not average occupancy or speed alone. Normalizing units to cell/reference-mass scales is permitted if conversions remain explicit.

Require a representable interval for every sum plus a useful quantization error bound. Overflow is a failed configuration/tick, not saturation that “keeps the simulation stable.” Guard the allowed input/concentration envelope before accumulation or detect unsafe range with a scheme whose own arithmetic cannot overflow. Do not assume 64-bit atomics are a portable escape hatch.

For repeatable integer sums, quantize each contribution under the documented rule before the order-independent reduction. Summing floats within a nondeterministically populated bucket and converting afterward changes that contract. GPU ledger reductions may use multiword integers or bounded partials; their ranges must also be proven. Full bitwise equality is checked by canonical IDs, not buffer allocation order.

## PB-MPM implementation algorithms

### Reference schedule

PB-MPM is ESTABLISHED; this sparse PPR integration is PROPOSED. The paper refines candidate displacement through its iterations, then integrates positions and deformation once. It does not advance physical time or apply gravity on every iteration.

1. Apply accepted commands and activation, establish the timestep's particle positions and bucket membership, and initialize candidate motion using forces once.
2. Clear touched grid pages; solve the material constraint, P2G scatter, finish the grid update/boundaries and G2P reconstruction for each reference iteration.
3. Keep reference particle positions fixed while iterating the candidate motion. Integrate position and persistent deformation once after the final solve; perform the adopted collision recovery and velocity reconstruction consistently.
4. Rebucket/rebase moved particles, commit state, update occupancy/visual data and diagnostics. Checkpoint capture sees a committed tick only.

The reference uses separate passes and FP32 particle state. Its exact initial/final transfers are part of the cost model. CPU validation covers a small tank and adversarial cases; GPU reference and optimized schedules compare equivalent iteration semantics, not just matching loop counters.

### Sparse pages and bucket construction

Use 8 × 8 ownership tiles as the first candidate, backed by a bounded page pool. A dense coarse address table is permitted: 512 × 512 u32 entries are 1 MiB of metadata, not a dense full-world solver grid. Compare that simple indirection with existing facilities before inventing a hash table. Active tiles cache required neighboring page indices; their construction and reads are accounted for.

Baseline bucketing is count → prefix-scan ranges → scatter particle indices. Reset touched counters; compact an active work list; dispatch indirectly without CPU particle-count readback. Allocate stencil-support pages, including halos, before executing. Handle large buckets by multiple bounded work chunks; N = 4 is reference sampling, not a hard instantaneous occupancy limit.

Atomic insertion need not produce a stable order. Keep stable particle identities, and make order-sensitive sleep packing, allocation-dependent behavior and audits canonical. Validate replay before claiming deterministic results. Initially rebuild buckets once per physical step, not per iteration while positions are fixed.

A full radix sort is optional. If it beats bucketing plus indirection/reordering, sort key/index pairs, then move hot state once. Tile addresses need 18 significant bits at the present world dimensions; extra key fields must justify extra digits. Compare portable multipass radix based on FidelityFX-style histogram/scan/scatter. Defer Onesweep/decoupled-look-back until backend memory-order and forward-progress requirements are established. Persistent membership/local migration is a later optimization with displacement bounds and stale-membership tests.

### Fused transfers and shared memory

After the reference passes, evaluate EA's G2P → constraint → P2G fusion across adjacent iterations, using separate source/destination grids and explicit first/last-step handling. Stage source grid patches in groupshared memory, reconstruct a particle into registers, solve its constraint, and scatter into local integer accumulators before flushing aggregated contributions.

A workgroup barrier synchronizes only that group. Complete cross-tile grid publication before dependent reads in a later dispatch. Do not perform all iterations independently inside tiles with stale halos. Particle data can remain local within one fused operation, not indefinitely across global synchronization.

Baseline flush uses atomics for all potentially shared nodes. “Only halo nodes require global atomics” becomes an optimization only after proving unique interior writers even when one bucket spans several groups. Read-only halo grid updates may be duplicated if semantically equivalent; body impulses and other side effects must have a unique owner.

For an illustrative 10 × 10 patch with four 32-bit fields, one grid stage is 1.6 KB; source/destination stages, extra model fields, padding and scratch add to that. Derive the actual halo from the stencil and indexing convention. Benchmark 64/128/256-thread work chunks and at least the nearby tile-size alternatives where justified; measure register spills, occupied waves, LDS bank conflicts and partial groups. Do not assume the smallest byte count yields the fastest kernel.

Use wave scans/reductions for list compaction, histograms and shared destinations; aggregate P2G only among lanes targeting the same node. Support wave32/wave64 or a groupshared baseline without relying on a fixed 32-lane mask. Newer intrinsics such as WaveMatch are capability-gated, not minimum requirements. Specialize by material model only if divergence savings exceed the extra partitioning/dispatch cost.

## Terrain, sleeping and checkpoints

### Terrain collision and edits

Build a narrow-band SDF from the same contour as terrain rendering and Box2D chains. Derive dirty-region expansion from the stored distance band plus sampling/interpolation footprint; “one neighboring cell” is not assumed sufficient for a ±2-cell band. Derive gas face apertures/connectivity from the contour itself, not min(cell capacity), which cannot describe every partial-cell opening.

Use grid normal projection with the adopted friction model and final particle recovery. Validate swept motion against thin and diagonal surfaces; an endpoint SDF test and velocity clamp alone do not prove no tunnelling. The total displacement bound must include constraint correction, drift if later adopted, terrain recovery and moving-body sweep. Handle degenerate/zero SDF gradients explicitly.

Dig/build commands preflight affected matter, slot and gas capacity before committing. Digging wakes affected support-connected matter. Building either relocates material through valid connected free space under a bounded deterministic policy or rejects the edit without loss. A logged deletion is not a successful conservative edit. Async preparation is versioned and cancelled on incompatible world replacement.

### Sleeping baseline

Initial world-generation spans may remain compact slot bricks because they have no prior motion or constitutive history. Use four material slots per cell, a capacity mask derived from contour geometry and explicit occupied-slot counts. Material palette width is checked; the previous 8 B/cell layout is a candidate only when its palette/temperature representation actually fits.

For previously active matter, first retain full state in dormant GPU pages and remove it from iteration work. Build conservative interacting components over occupied tiles plus stencil dependencies; over-connecting costs performance but is safe. A component can sleep only after low motion, low residual/volume change and no pending source, contact or geometry disturbance for the adopted dwell time. Do not sleep low-speed particles that are still under-resolved or about to move.

Wake the complete interacting component before an external interaction reaches it, using terrain-change footprints, swept body bounds, sources and active-neighbor dependencies. Rebuild connectivity when topology changes; include that cost. Do not cap a wake by leaving physically connected material arbitrarily frozen. If the whole pool wakes, its whole cost is paid and the overload policy applies.

Partial-component sleeping is deferred until a support/pressure boundary formulation passes deep-pool and digging tests. The old “1.5 cells per iteration, therefore one brick is enough” proof is withdrawn: derive the full composed transfer and motion dependency radius. Sampling a sleeping region for occupancy or body probes is allowed and counted; sleeping means no iterative solver work, not literally zero memory traffic.

### Quantized settled storage and grid-only checkpoints

Runtime slot packing is a later M2 optimization for eligible liquids. It preserves counts/material identity, not positions, momentum, energy, stress or an exact boundary. Keep full dormant state when position/model-state tolerances fail. Granular/soft history is not silently discarded.

The accepted grid-oriented save direction remains: **checkpoints do not serialize particle trajectories, velocity or affine state for the initial liquid scope**. A checkpoint worker packs a consistent committed snapshot; runtime matter is not moved merely to save.

1. Bucket snapshot particles by cell; include existing settled slots exactly once.
2. Assign valid slots deterministically with stable-ID tie-breaks; use a fixed material-density/position order and explicit contact/topology rules.
3. Relocate excess only through connected permissible free cells within a bounded search, never through terrain. Record displacement error.
4. Preserve unresolved excess in sparse cell records containing material ID and integer particle-equivalent count for each material. A single 4-bit excess count is insufficient. Save allocation/size overflow fails the save without replacing the previous checkpoint.
5. Load validates ledger, capacity and version, reconstructs supported liquid samples at rest, and runs the same collision/relaxation path. Reject an unsupported compressed configuration visibly rather than overlap arbitrarily or delete matter.

Packed save, wake and re-pack must be idempotent for the declared supported fixtures; per-material counts including excess are exact. Test height/surface changes, not merely particle counts. Bound admitted unresolved compression with load tests. Unsupported cases may fail checkpoint creation; “reported mass loss” is not success.

Granular persistence is a separate M3 gate: define cell-oriented constitutive sidecars or explicitly restrict the supported material model/checkpoint states before exposing it in persistent worlds. Grid-only saving does not authorize resetting arbitrary plastic strain. Thermal persistence waits for M8.

The former 352 MiB absolute save bound is withdrawn because species records, constitutive history and excess data were omitted. Report real sparse storage, worst admitted snapshot expansion and checkpoint staging separately. Compression is optional after correctness; it cannot make an incomplete representation conservative.

## Gas and its initial matter coupling

### One initial representation

Choose rung 0 only: one species per active cell, fixed integer mass quantum and uniform temperature T = 293 K during the isothermal milestones. This is a cellular gameplay model, not a multi-species compressible-fluid solver. Unlike species cannot mix within a cell; pressure equilibration across arbitrary unlike-species interfaces is not promised.

Baseline cell storage is 8 B: u32 mass quanta plus one species/flags word. A candidate quantum q = 2^-20 kg gives about 0.954 mg resolution and just under 4096 kg per u32 cell. It is a starting range/precision experiment, not a frozen save constant. A 24-bit mass at that quantum would cap a 4 B species/mass cell at just under 16 kg; test whether gameplay sources and admitted pressure ranges fit before adopting it. Zero mass has a canonical empty ID.

Use the ideal-gas relation only to derive a diagnostic/local transport pressure for positive, fixed cell capacity:

```text
p = (mass_kg / molar_mass_kg_per_mol) * R * T / gas_cell_volume
```

Convert catalog g/mol to kg/mol once. At 1 atm and 293 K, a full 1/9 m³ air cell contains about 0.13 kg. No hidden FP16/logarithmic mass encoding is used for the ledger.

### Conservative pair transport

Use four disjoint face phases: horizontal-even, horizontal-odd, vertical-even, vertical-odd. Each pair owns both cell writes within its phase; global boundaries between phases make later pairs see committed earlier-phase state. Alternate the phase order deterministically across ticks to measure/reduce directional bias. Tile boundaries use global face parity, not independent brick parity.

For each open pair:
- Same species, or one empty cell: derive a signed pressure-driven transfer; clamp it to the donor's available integer quanta and receiver's representable capacity. Quantize one shared transfer amount, subtract/add exactly that amount, and assign/clear species IDs consistently.
- Unlike species: no partial mass exchange in rung 0. A vertical whole-content swap may implement density ordering, with a density margin, cooldown and capacity checks. With unequal cell volumes, recompute both resulting densities/pressures and reject an inadmissible swap.
- Closed face or no admissible transfer: preserve state and record blocked-transfer statistics. Do not discard an unrepresentable inflow or overwrite a species.

Use a face-local pressure/gravity drive such as delta-p + w * rho_face * g * delta-y with a defined sign convention, mobility units and a hydrostatic-rest test. Do not use p + w*rho*g*y as a general potential: with spatially varying rho, it makes flux depend on the arbitrary vertical origin. Keep w = 1 for the baseline same-species hydrostatic test; exaggerated unlike-species pooling uses the explicit swap rule, not a claimed physical diffusion law.

Donor limiting proves nonnegative mass, not monotonic pressure convergence. Choose transfer mobility/relaxation from the two-cell update and volume bounds, then test equalization without overshoot. Tiny sub-quantum flux can stall; quantify the equilibrium error before adding residual storage. Avoid “fixing” it with nondeterministic random transfers.

Sparse scheduling lists only participating faces/cells and the frontier into empty neighboring cells. Every conservative exchange wakes its recipients. Sleep when all admitted fluxes and source changes remain below thresholds for the dwell time. A global half-rate gas step is an option because this initial model has no stiff two-way mechanical pressure coupling; include source latency and gas transport speed in its gate.

The M4 gate covers same-species vent equalization and unlike-species vertical pooling. If required gameplay needs pressure-driven displacement or mixing through unlike-gas interfaces, this representation fails that requirement: return to a specific design choice, such as fixed K = 2, rather than building three rungs speculatively or declaring arbitrary ventilation solved.

### Matter occupancy and displaced gas

M5 initially treats gas availability as a hysteretic open/closed cell classification from terrain, body and matter occupancy. Use thresholds and a minimum admitted gas volume; do not continuously divide by a noisy free fraction approaching zero. Derive face connectivity from actual geometry. A closed cell is outside the active pressure field.

When a cell closes, move its gas through valid connected openings to admissible same-species or empty cells with deterministic bounded searches/reservations. Plan the moves before committing them so multiple donors cannot overfill one receiver. If no recipient exists, retain exact species/quanta in a cold displaced-gas record anchored to the cell/region. Retry on relevant opening events, not by scanning all blocked records every tick.

Displaced records contribute to the mass ledger but have no invented physical pressure, breathable volume or bubble velocity. Probes return an explicit blocked/displaced status. This is a conservative bookkeeping fallback with a visible gameplay limitation: **sealed air does not mechanically resist flooding in the initial scope**. It is not a diving-bell or entrainment model. Reopening releases records only into valid capacity under the same conservative transport rules.

Gas-to-matter pressure force, U-tube pressure matching, entrained bubbles and gas-pocket compression are deferred research. Before adoption, specify the minimum-volume treatment, timestep/energy bounds, displacement work, pressure extension and coupling tests. A mass limiter or force clamp alone cannot prove that coupling stable.

Sources/sinks and up to 256 requested probes use tick-stamped commands and bounded output storage. Validate accepted quantities before mutation and return rejected capacity/species requests explicitly. Probes report their originating tick and representation status, not a guaranteed one-tick freshness.

## Stratification, granular matter and rigid bodies

### Early stratification experiment and M3 decision

A single grid velocity can suppress unresolved relative phase motion, but it does not prove that all spatially resolved density-driven buoyancy is impossible. First test equal-reference-volume oil/water with correct mass weighting and the unmodified PB-MPM reference: inverted layers, a droplet, an already stable interface and an isolated translating/rotating configuration. Separate volume error from actual phase-separation failure.

The earlier particle drift is not adopted. Subtracting a gathered nodal mean from particle velocities does not produce an exact nodal volume-neutral projection. As a simple two-particle/two-node illustration, weights (0.75, 0.25) and (0.25, 0.75) with drifts +1 and -1 leave nodal corrected fluxes +0.375 and -0.375 after that subtraction. Density-only drift can also keep acting on an already stable interface.

If the unmodified solver fails the required stratification scenes, M3 stops production expansion and tests one bounded two-liquid extension. First candidate: sparse phase-specific momentum/velocity fields only on mixed-support nodes, with explicit interphase exchange, shared occupancy/contact and a defined pressure/constraint partition. Conserving a volume-weighted slip sum at a node is necessary but not a complete solver; specify momentum/energy exchange and particle transfer before implementation.

This candidate requires a separate derivation and reference implementation gate. Price extra node fields, transfers and mixed-region expansion before adoption. Do not silently generalize to an arbitrary per-material velocity grid, or substitute ad hoc position drift to make a scene look right. Stratification remains required for a completed multi-liquid phase even while unrelated one-liquid work proceeds.

Add one granular model only after liquid contracts pass. Use a documented 2D deformation/plasticity model, with its F/history sidecars and stable constitutive projection. The inspected EA implementation is a reference, not proof that arbitrary sand/snow/clay parameters share one scalar state. Validate pile rest, friction/angle of repose, shear, compression/release, liquid interaction and persistence. Defer broad viscoplastic/soft-material catalogs.

### Body geometry and analytic fluid forces

The earlier source inspection found box/circle body descriptions. Recheck current `BodyDefinition` and `BodyState` before implementation. Game owns the body definitions and publishes handle generation, pose, prior pose and velocities; matter builds/consumes tile-local body lists. Separate dirty shape data from frequently updated poses. Do not duplicate every body into every particle.

Bodies drive the grid as kinematic colliders, with swept bounds and relative normal/tangential response. Assign one body/sample owner when halos or work chunks overlap. Overlapping bodies need a deterministic contact policy; do not apply the same correction or force twice.

Initial fluid-to-body feedback is analytic Archimedes plus drag. Estimate submerged area and centre of buoyancy from an exterior reconstructed liquid field and quadrature of the body shape. Do not sample only interior nodes after collision has excluded fluid there: that would report an empty body and no displacement. Compare against known submerged boxes/circles and surface-crossing cases.

Accumulate displaced mass, force and torque about the body's centre of mass with a workgroup reduction and bounded per-body output. Use drag opposing relative motion; test its discrete damping behavior and cap an impulse relative to the available relative momentum/effective mass rather than treating “10 × body weight” as a stability proof. This remains an approximate delayed coupling and needs light-body/high-drag tests.

Apply tick-stamped results at most once via the physics seam, with generation checks. Define a maximum accepted result age; expired results are dropped with a diagnostic, never re-applied. If no fresh sample exists, initially apply no new fluid impulse and report the gap rather than blocking the CPU or integrating a stale impulse repeatedly. Real latency depends on GPU completion; fixed one-tick latency is not guaranteed. Continuous gaps fail the platform/behavior gate.

Do not add liquid grid-reaction impulses on top of analytic buoyancy/drag. Granular/soft reaction is a separate deferred extension requiring mass-share attribution, unique grid ownership and energy/latency tests. Static terrain/body contact and body/body dynamics remain Box2D responsibilities. Direct-control dupe behavior is still deferred.

## Implementation phases and gates

Phases below refine the original M0–M8 names. The early probes are intentionally small; they challenge consequential choices before storage/coupling infrastructure grows. Gates record correctness, cost evidence and TARGET-PENDING independently. A failed feature blocks its dependent work, not unrelated already-approved one-liquid development.

### M0 — analytical contract, native reference and early feasibility

**Algorithm:** CPU reference and simple Slang FP32 PB-MPM tank, N = 4; fixed-position iterations and one final integration; explicit active bounds, basic integer P2G with range/error analysis and per-pass timestamps. Use existing RHI/shader facilities and a scalar/groupshared-compatible path. No wave feature or packing is required for correctness.

**Work:** enumerate fields, pass dependencies, transaction buffers and budgets; use P/G/T sweeps and 2/3/5-iteration comparisons. Add 10 k-particle CPU/GPU comparisons, a deep pool and a dam break. Run the oil/water probes and a simple prescribed box/displacement probe now; record unsupported cases rather than grafting drift or pressure heuristics onto the reference.

**Gate:** exact ledger over 10^5 validation ticks; no invalid state or range overflow; reference agreement under stated quantization tolerances; replay hashes in canonical identity order; bounded volume loss/rest motion; complete allocation and logical-byte ledger. Record unresolved multi-liquid findings. All timings are development observations. Target devices are not a prerequisite.

### M1 — terrain and sparse, locality-aware execution

**Algorithm:** contour SDF/apertures, bounded sparse page table, count/scan/scatter buckets, active indirect dispatch and swept/recovery collision. Compare split passes against fused G2P/constraint/P2G with groupshared grid patches. Start with all shared-node flushes atomic; specialize only after ownership proofs.

**Work:** tune group/tile sizes, benchmark compact tanks versus spray, compression/overfull buckets and overlapping body probes. Test one packing candidate at a time against FP32. Add committed-tick resource exhaustion and cancellation cases. Only adopt radix sorting if the complete bucket/sort-plus-solver timing improves.

**Gate:** cave dam break, thin/diagonal wall and rapid-edit tests; contour/body/particle resting surfaces agree within a declared geometry tolerance; no lost particles during tile migration; fused/reference agreement; sparse cost follows active pages; spills/atomics/barriers and full tick traffic reported. No million-particle target claim or hardware-before-M2 gate.

### M2 — conservative sleeping and liquid checkpoint packing

**Algorithm:** component activity over tile/stencil connectivity, full-state dormant pages and complete-component wake. Initial authored slots expand on demand. Separate checkpoint worker packs a consistent committed liquid snapshot into slots plus typed integer excess records.

**Work:** test cold-page residency and memory pressure; compare full-state sleeping with optional eligible-liquid slot quantization. Measure connectivity rebuild, wake bursts, snapshot staging and worst admitted expansion. Leave internal reservoir freezing disabled.

**Gate:** sleeping/awake comparison for pool rest, digging and approaching bodies; no invented support boundary; all interactions wake the required component before use; exact counts/material identity; save cancellation preserves the previous checkpoint; pack/load/repack idempotence on supported fixtures; failed excess/capacity cases are visible. Report geometry/motion loss separately. Target performance remains pending.

### M3 — liquid stratification and one granular model

**Algorithm:** extend the accepted reference only from the M0 evidence. Validate unmodified multi-density PB-MPM first. If needed, evaluate the bounded mixed-node two-liquid formulation through an explicit design gate before production edits. Add one documented granular constitutive model with specialized sidecars.

**Work:** measure extra bytes/nodes/dispatches from mixed phases and model divergence; preserve a single-liquid fast path if it pays for its partitioning. Specify granular sleep and cell-oriented persistence without discarding required history.

**Gate:** inverted layers separate; stable layers remain quiet; isolated translation/rotation does not acquire spurious drift; phase masses exact; compression and surface error bounded; granular pile/shear tests and declared persistence pass. Passing a visual separation scene alone does not pass momentum/volume/rest tests. If the formulation is unresolved, M3 is incomplete, not silently replaced with the rejected drift.

### M4 — cellular gas only

**Algorithm:** one-species integer cells, four disjoint conservative face phases, same-species/empty-cell transfer, guarded vertical unlike-species swaps, sparse activity/frontier and uniform isothermal state. Geometry and capacity checks precede writes.

**Work:** quantify integer equilibrium error, phase-order bias, pressure relaxation, blocked unlike-species faces and wake propagation. Compare 8 B baseline with the checked 4 B candidate. Compare a global 30 Hz gas step with 60 Hz. Do not build rungs 1/2/3 pre-emptively.

**Gate:** exact species ledger; no negative/overflowing mass; same-species vent equalization is monotonic under the adopted coefficients; sealed uniform room rests; vertical pooling is bounded and repeatable; coordinate-origin invariance; cross-tile parity and capacity tests. Record the unlike-interface ventilation limitation. A gameplay scene requiring behavior outside it triggers a specific representation decision.

### M5 — occupancy coupling, displacement and probes

**Algorithm:** hysteretic cell availability, geometry-derived open faces, deterministic bounded relocation with receiver reservations, cold displaced-gas records, tick-stamped probes/sources. Matter does not receive gas pressure forces in this milestone.

**Work:** test flooding/opening, rapidly alternating occupancy and multiple donors; ensure retries are event-driven. Publish distinct active-gas and displaced-gas status. Add a minimal dirty visual plane so rendering/GI does not read full particle/gas state.

**Gate:** exact cross-representation gas ledger; no overwrite or duplication; disconnected rooms do not exchange through solid geometry; reopening conservatively releases retained gas; probe age/status is correct; occupancy hysteresis does not lose material. The initial flooding scene does not claim U-tube, bubble-rise or diving-bell physics.

### M6 — Box2D analytic fluid coupling

**Algorithm:** game-owned shape/pose table, tile-local broad phase, kinematic grid collision, exterior liquid reconstruction and body quadrature; aggregate Archimedes/drag forces and torque; completion-governed readback with at-most-once generation-checked application.

**Work:** tune spatial sampling against known shapes; separate shape-cache changes from pose uploads; test delayed/missing results, light bodies, high damping and overlapping tile halos. Include body occupation in gas displacement and sleep wake footprints.

**Gate:** expected box submerged fraction and circle displaced area; heavy body sinks; long plank reaches stable orientation; still-water force/torque balance; dissipative drag/terminal-speed behavior within adopted tolerances; stale handles/late results/capacity and shutdown paths. No granular reaction or precise splash impulse claim.

### M7 — integrated scale, overload behavior and platform acceptance

**Algorithm:** compose the accepted passes; use dirty lists, cached topology with explicit invalidation, compact GPU work lists and bounded allocations. Fuse ledger/visual reductions into existing unique-owner passes where this preserves validation visibility. Avoid full-world scans for diagnostics.

**Work:** representative game + GI scenes: dense lake, fragmented spray, deep reservoir, several distant simultaneous disturbances, widespread dig/build, gas-filled caves, moving bodies, wake storm and checkpoint during motion. Compare reference/optimized profiles and instrumentation overhead. Budget both steady state and bursts.

**Gate:** all adopted feature gates remain passed under combined load; no unbounded resource growth; timing distributions and resident/transient memory fit the adopted product budgets on measured hardware. Exhausted capacity fails/defer-rejects the responsible command or tick visibly before partial publication. Never drop matter, freeze arbitrary camera-distant bricks, or run unbounded catch-up.

Without target hardware, deliver the integrated development evidence and mark target timing/caps TARGET-PENDING. With hardware, validate Deck and One S separately; success on one does not certify the other. A lower uniform simulation rate or N = 1 is a new behavioral profile with full affected tests, not an automatic emergency optimization.

### M8 — separately adopted extensions

Thermal transport, latent heat/phase changes, richer gas mixtures, trapped-gas pressure/entrainment, broad soft materials, partial-component sleeping, granular reaction feedback and multirate integration are outside the initial acceptance scope. Each needs its own state/traffic additions and behavior gate. The basic visual/GI bridge already belongs to M5/M7 and is not postponed with thermal work.

Phase changes must reconcile particle mass quanta with gas quanta without hidden remainders. Richer gas state must earn its cost in required gameplay. A two-way pressure coupling must establish rest, compression, energy and timestep behavior before adding bubbles. Per-brick tick skipping requires a conservative multirate scheme across shared nodes; adjacency rate limits alone are insufficient.

## Diagnostics, lifecycle and stop criteria

### Shipping counters versus validation audits

Accumulate cheap counters with work already being done: active P/G/T, bucket occupancy, particle/slot transitions, accepted sources/sinks, displaced records, range failures, collision recovery, clamp hits, blocked flux, readback age and allocation high-water marks. Maintain exact ledger deltas at mutations. Validate those incremental ledgers against full independent reductions in validation scenes and at save/load boundaries.

Do not scan all sleeping cells every tick merely to prove they are asleep. Full NaN/state-hash/mass audits are validation modes with their overhead reported separately. Per-pass GPU timestamps and useful aggregate CPU profiling scopes remain; instrumentation overhead must not be included in an uninstrumented speedup claim.

### Failure, cancellation and teardown

All CPU commands and GPU packets carry world/tick/version identity. Check resource/format compatibility before accepting work. Completion gates reuse of scratch, staging, body tables and readback slots. A pending checkpoint owns a consistent snapshot or copied staging, not references to mutable particle buffers.

On world replacement/shutdown: stop new submissions and callbacks, cancel/wake/drain outstanding jobs and GPU work as appropriate, retire readbacks, then release resources in dependency order. Preserve the first cleanup error while attempting independent cleanup. Pool growth is bounded and observable; allocation failure never becomes an empty “successful” simulation.

### Risks and explicit decisions

| Risk | Required response |
| --- | --- |
| PB-MPM volume error at affordable iterations | Compare depth/impact scenes and uniform dt/iteration profiles; do not claim stiffness retuning fixes convergence |
| Active region exceeds available cost | Report complete-region cost and scope limits; evaluate an explicitly adopted global profile. A cell-based liquid transport redesign is a future product decision, not authorized by this plan |
| Sleep removes support or causes pops | Retain full state and sleep whole interacting components; leave partial sleeping/slot conversion disabled |
| Multi-liquid separation fails | Stop M3 expansion; derive/test the bounded two-liquid candidate and account for its added grid state |
| Packing changes creep, rest or volume | Reject that field's quantization; preserve independently successful packing |
| Integer range/determinism fails | Correct units/range/input envelope or choose a separately validated reduction; do not saturate physics or claim integer sums alone solve replay |
| Gas interfaces block required gameplay | Make the limitation explicit; select one justified richer representation through a new decision |
| Sealed displaced gas needs mechanics | Adopt a separate pressure-coupling design; bookkeeping relocation does not satisfy that feature |
| Delayed body feedback oscillates | Test age/damping/impulse policy; restrict supported coupling before claiming stable two-way behavior |
| Console/platform path is unavailable | Continue platform-independent development; deployment and target acceptance remain pending |
| Save format cannot preserve an admitted state | Preserve typed excess/history or fail snapshot creation without replacing the last valid save; no counted mass loss as success |

## Supersession and current decisions

The historical log below is preserved verbatim for rounds 0–4. Its older statements describe the proposal at that time; this revised body and the appended decisions supersede contradictory requirements.

| v1 / earlier v2 area | Current action |
| --- | --- |
| v1 adaptive hierarchy, kernels, split/merge and affine coarse levels | Removed; fixed-resolution PB-MPM and tile grouping |
| v1 unified gas particles | Replaced by cellular gas |
| v1 particle rigid clusters | Replaced by Box2D |
| Earlier v2 universal packing and capacity estimates | Replaced by explicit hot/cold layouts, conditional quantization and complete-pass accounting |
| Earlier v2 drift, singular gas pressure and guaranteed one-tick feedback | Withdrawn; early stratification gate, limited occupancy coupling and completion-aware feedback |
| Earlier v2 lossless sleep and fixed save-size bound | Replaced by full-state sleeping plus explicitly lossy geometry packing with exact typed material counts |
| Earlier v2 target-before-M2 / desktop load multiplier gate | Replaced by design/development acceptance now and independent TARGET-PENDING acceptance |
| v1/v2 diagnostics and staged gates | Retained with audit overhead and measurement scope explicit |

Decided for implementation planning: keep PB-MPM; keep dx = 1/3 m and N = 4 initially; separate gas and Box2D; simplify the initial coupling; treat bandwidth and capacity as measured constraints; preserve grid-oriented liquid checkpoints; defer dupe control and broad soft/thermal mechanics. Correctness work may proceed without target hardware.

Open product/design decisions are: the adopted CPU/GPU/memory budgets and maximum simultaneous gameplay disturbance; native/Proton Deck deployment and Xbox SDK/RHI route; 30 Hz acceptability if needed; exact admissible save displacement/compression bounds; gas behavior needed at unlike-species interfaces; the outcome of the early stratification experiment. These do not authorize silent changes to the selected algorithms. Record resolutions in the log.

## References and implementation reading order

Reviewed primary sources support individual techniques, not PPR target performance. At implementation, pin the inspected upstream revision and preserve required notices.

1. [Lewin, A Position Based Material Point Method](https://media.contentapi.ea.com/content/dam/ea/seed/presentations/seed-siggraph2024-pbmpm-paper.pdf): candidate-displacement iterations, single integration, relaxation/convergence limitations. The paper's stability discussion does not prove this plan's extensions.
2. [EA PB-MPM](https://github.com/electronicarts/pbmpm): readable SIGGRAPH 2024 branch for semantics, optimized main for comparison. [Fused shader](https://raw.githubusercontent.com/electronicarts/pbmpm/main/shaders/g2p2g.wgsl), [particle/model state](https://raw.githubusercontent.com/electronicarts/pbmpm/main/shaders/particle.inc.wgsl), [bucket count](https://raw.githubusercontent.com/electronicarts/pbmpm/main/shaders/bukkitCount.wgsl) and [insertion](https://raw.githubusercontent.com/electronicarts/pbmpm/main/shaders/bukkitInsert.wgsl).
3. [Fei, Huang and Gao, GPU-MPM principles](https://arxiv.org/html/2111.00699v1): kernel organization, sorting frequency, register/shared-memory tradeoffs. CUDA/V100 results are not Deck/GCN predictions.
4. [AMD RDNA performance guide](https://gpuopen.com/learn/rdna-performance-guide/): wave sizes, LDS layout and occupancy considerations; distinguish GCN capabilities.
5. [FidelityFX Parallel Sort](https://gpuopen.com/fidelityfx-parallel-sort/), [Onesweep paper](https://arxiv.org/abs/2206.01784), [GPUSorting implementation notes](https://github.com/b0nes164/GPUSorting): portable multipass baseline versus progress-sensitive optimization.
6. [Slang standard library](https://shader-slang.org/stdlib-reference/) and [Microsoft Shader Model 6.5](https://microsoft.github.io/DirectX-Specs/d3d/HLSL_ShaderModel6_5.html): capability checks, including WaveMatch. Compilation, reflection, pipeline creation and execution are separate portability checks.

## Decision log: how the plan was built

Questions, answers and their effect, kept as evidence. Quotes are the user's words, lightly trimmed. Append new rounds; do not rewrite earlier rows.

| Round | Date | Question or input | Answer | Effect on the plan |
| --- | --- | --- | --- | --- |
| 0 | 2026-10-13 | Brief | Adaptive simulation is too complicated because boundaries cannot be conserved when merging or splitting circles. Gas has negligible mass but PB-MPM still locks gases inside other elements. Buoyancy is important, as are collisions with the static scene and Box2D rigid bodies | Fixed resolution; gas field; explicit buoyancy; SDF and Box2D coupling |
| 1 | 2026-10-18 | Q1: grid spacing and particles per cell | "yes, what the plan assumed, even 1/3m might be complicated later: maybe 1/4 would simplify numerical stability" | dx = h and N = 4 kept; note on dx = h/2 versus 1/4 m |
| 1 | 2026-10-18 | Q2: should liquids stratify? | "yes, liquids should stratify" | Drift required; M3 gate mandatory |
| 1 | 2026-10-18 | Q3: should gas pool like ONI? | "yes, gas should pool like ONI" | Large w, tuned in M4 |
| 1 | 2026-10-18 | Q4: gas species cap and memory | "scaling with the number of elements could be an issue, this is why I wanted originally a particle based simulation: this is main risk of the plan for me" | Cost-scaling subsection; top risk in the register |
| 1 | 2026-10-18 | Q5: what do checkpoints store? | "I would be tempted to project back all the elements to the grid for serialization, and never store particles directly"; risk named: several elements in one cell must be packed into different cells | Grid-only serialization; `packToSlots` primitive |
| 1 | 2026-10-18 | Q6: which body shapes exist? | "I really don't know, but the code is open-source and available for you to check" | Checked: box or circle only; `applyBoundedForce` has no clamp parameter; no drag field |
| 1 | 2026-10-18 | Q7: dupe in fluid | "ignore the dupe coupling for now" (direct control or ONI-style orders undecided) | Deferred, outside all gates |
| 1 | 2026-10-18 | Q8: reference thickness | "maybe? what do you recommend" | Recommendation: keep 1 m (awaiting confirmation) |
| 2 | 2026-10-18 | Open Q1: oil and water without drift in v10.2? | "I don't know" | Step 0 of M3 |
| 2 | 2026-10-18 | Open Q2: matter hanging at rest after load | "Yes, and worst case scenario we can warmup the simulation" | Accepted; warm-up fallback added |
| 2 | 2026-10-18 | Feedback on per-cell multi-material storage | "I fear that this new formalism would drastically impact performance due to bandwidth" | Gas ladder, bandwidth estimates, start at rung 0, M4 decision rule; new open question on target GPU |
| 2 | 2026-10-18 | Instruction on the plan itself | "keep an evidence of our back and forth questions in the plan ... in addition to all pre-existing instructions" | This log; Instructions for coding agents section |
| 3 | 2026-10-18 | Open Q1: target GPU class | "I need to be able to run on integrated graphics and laptop, this is critical as the game is heavily focused on simulation at interactive rate" | New performance-budget section; packed particle format; 2 iterations on integrated GPUs; gates measured on a reference integrated GPU; N reconfirmed before M2; new open questions on reference devices and 30 Hz |
| 4 | 2026-10-18 | Open Q1: reference devices; M0 microbenchmark preamble | "steam deck and xbox one s, my actual pc is a amd 3950x with 2080 gpu and 64gb -which is not really representative of the market and quite aged, but it should help not overshoot the target. About M0 microbenchmark preamble: it's a nice idea but I don't have yet the target hardware at disposal" | Reference hardware named; bytes-first method, load-multiplier proxy and first-Deck checkpoint replace the M0 microbenchmark; new open questions on route to targets and Deck acquisition |
| 5 | 2026-10-18 | Performance and stability review | User requested an honest review for Steam Deck and Xbox One S, including shared memory, wave operations, radix sorting and simplification where risk is high | Retain PB-MPM; reject optimistic capacity claims and hardware emulation by desktop load multipliers; identify coupling/sleep/precision risks and reuse EA's bucketed fused-transfer implementation as a reference |
| 6 | 2026-10-18 | Adopt review in v2 | "Update the v2 plan with these recommendations. Keep PB-MPM, correct the bandwidth and stability claims, simplify the initial scope, and revise M0 so we can proceed without target hardware. Refine each phase implementation algorithm choices. You should also identify cold / hot storage optimization opportunities, as well as data packing/quantization: bandwidth is critical. Preserve the decision log." | Supersedes earlier mandatory drift, singular gas coupling, lossless-sleep claims, universal 20 B/135 B estimates and target-before-M2/load-multiplier gates. Adds detailed M0–M8 algorithms, hot/cold and model sidecars, checked 40 B/24 B liquid and 8 B/4 B gas candidates, conservative typed excess, whole-component sleeping, analytic body forces, limited gas occupancy coupling, and separate design/development/target acceptance; rounds 0–4 preserved |
