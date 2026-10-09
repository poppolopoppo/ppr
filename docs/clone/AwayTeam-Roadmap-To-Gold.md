# Roadmap: current PPR → commercial Gold

> **Document scope:** Design/proposal record, not a current implementation inventory or repository-wide instruction. Apply its task-specific requirements only when the active task explicitly adopts this design. Verify implementation claims against source/CMake and the current [documentation guide](../README.md); historical paths, counts and phase status may be stale.

**Recommended sequence:** P0 feasibility → MVP-1 **Breathe** → MVP-2 **Plumb & Burn** → **Content-complete Beta** → **Polish** → **Gold**.

MVP proves the complete survival loop; it is **not** the final product. Each gate produces a packaged, runnable build, reproducible evidence, and an explicit go/no-go decision. P0 is an internal technical release, not a public product.

This roadmap uses the accepted clone contracts. It supersedes their old “MVP → Multiplayer” delivery sequence: **neither DLC nor networked co-op is required for Gold.**

## 1. Decisions that govern the entire roadmap

### Preserve these boundaries

- `engine.sim`: renderer-independent grid, scheduling, state, and checkpoint primitives.
- Game layer: Away-specific elements, recipes, objectives, survival rules, avatar control, and orchestration.
- Client/presentation: camera, staged tools, overlays, UI, audio/VFX presentation.
- Renderer: content-free orchestration; passes own their rendering resources.
- Persistence: snapshot codec remains separate from filesystem operations and save-slot UX.

Do not turn the current demo host into the permanent home of every system. Extract cohesive game responsibilities **as they ship**, not through a speculative framework rewrite.

Keep the existing single-threaded ECS and simulation initially. Introduce worker execution only after measurement demonstrates a requirement; never make simulation work selection depend on elapsed wall-clock time.

### Resolve these contract details at P0

1. **Use the accepted four-field Cell.** The architecture phase table’s “Cell sidecar prototype” wording must not revive a second payload or mixture model.
2. **Publish one executable schedule mapping.** Map canonical S00–S16 to concrete registered steps and the existing locomotion sequence. Preserve the specified ordering, previous-tick power semantics, integer divisors, and single damage path.
3. **Treat 27 entities as minimum implementations, not final content volume.** MVP-2 covers the inventory’s required entity families; Beta completes their production content, variety, balance, and presentation.
4. **Freeze a commercial launch scope.** “Full product” means the approved Away scope, not every system cut from ONI.
5. **Start IP clearance immediately.** Reference mechanics do not authorize copied characters, names, artwork, audio, dialogue, or branding.

---

## 2. Gates shared by every phase

### Build and engineering gates

Every exit requires:

- Clean builds on the approved compiler/platform matrix; at least MSVC and supported Clang coverage for the initial Windows target.
- Unit/integration tests in testing-enabled configurations **and** smoke/install tests of the actual shipping configuration. `msvc-rel` alone does not run the test suite.
- Appropriate core/sim, app, asset, shader, and GPU-readback coverage.
- Applicable sanitizer runs and GPU validation on supported configurations.
- Canonical formatting and touched-file semantic-style checks.
- Explicit ownership, injected dependencies, error-bearing APIs, inverse-order teardown, and partial-initialization failure tests.
- No undeclared placeholder systems: fail-closed `StepRegistry` remains meaningful.

Implementation follows the repository’s specialist authorities; this roadmap does not replace their procedures.

### Performance contract

Exact minimum hardware and absolute memory ceilings are **P0 decisions**, not existing measured facts.

Recommended initial targets to ratify or explicitly revise at P0:

| Metric | Proposed gate |
|---|---|
| Simulation | Sustained 60 Hz at normal speed; tick p95 ≤ 8 ms, p99 ≤ 12 ms |
| Presentation | 60 fps target at agreed resolution; frame p95 ≤ 16.67 ms, p99 ≤ 25 ms |
| Backlog | No sustained growth or silent loss of simulation time under supported workloads |
| Regression | Investigate/block unexplained >10% regression in a stable benchmark |
| Memory | Fixed ceilings for grid, solver scratch, checkpoint staging, GPU staging, assets, and total peak RSS |
| Stability | No continuing post-warm-up memory growth, cursor starvation, or tick-time drift |

These are independent measurements, not additive CPU/GPU budget guarantees.

A 4096² world must not imply that only the camera-visible region simulates. Measure:

- Fully resident grid.
- Typical active area.
- Maximum supported activity.
- Deliberately overloaded conditions.

Bounded cursors must preserve correctness and fairness. Record **oldest pending work age and complete-sweep time**, not just cheap ticks. P0 must set acceptable solver latency and demonstrate that budgeting does not create effectively frozen fluids or heat.

### Mandatory benchmark suite

Extend existing `--time-trials` S1/S2 and `--load100`; preserve their current cases.

Add deterministic scenarios for:

- Sealed-room gas settling and oxygen consumption.
- Breach across chunk boundaries.
- Liquid fall, pooling, pumping, and backpressure.
- Thermal gradients, phase transitions, and fire.
- Coupled power/plumbing/automation.
- Offscreen active regions and widespread activation.
- Concurrent save capture, edits, overlays, and camera movement.

Record tick/stage p50/p95/p99/max, activity backlog, mass/energy accounting, RSS/peak RSS, checkpoint bytes, and GPU upload volume. Unimplemented mechanics are explicitly marked unavailable until their phase—not simulated by empty benchmark work.

### Replay and save gates

At every phase:

- Same build + seed + pack manifest + tick-stamped intents → equal canonical authoritative state.
- Uninterrupted execution equals checkpoint/load/continue execution at defined comparison ticks.
- Hash semantic state, not padding, pointers, handles, timings, or presentation caches.
- Include scheduling cursors, RNG state, inventories, networks, objectives, and replay-relevant timers.
- Reject truncated, corrupt, incompatible, missing-pack, and wrong-ancestry input without modifying the live world.
- Maintain a checked-in compatibility fixture corpus.

This is **same-build evidence**, not a cross-platform lockstep promise. If physics restore cannot satisfy continuation equality, resolve that before declaring checkpoint fidelity.

---

## 3. P0 — feasibility spike and production contracts

### Entry

- Accepted gameplay, architecture, and UX contracts available.
- Existing PPR tests and benchmark results recorded.
- Candidate minimum hardware and launch platform identified.

### IN — prioritized implementation

1. Extend Cell and its validation; establish stable element identifiers.
2. Implement representative conservative gas/liquid transfers and a thermal kernel on test worlds.
3. Add `SimActivity`, neighbor wake, deterministic bounded traversal, and persistent cursors.
4. Establish canonical schedule mapping and instrumentation.
5. Implement snapshot-v5 baseline semantics, transactional restore, and explicit v4 migration tests.
6. Add heat/gas diagnostic views using palette remapping; preserve tint/cut-out/lighting separation.
7. Measure fully resident memory, scratch, checkpoint peak memory, and active-world cost.
8. Record base-pack identity/hash, intent-envelope, and checkpoint-ancestry contracts.

### DEFERRED

Production content, complete survival, final art/audio, networking, DLC loading, scripting, blending/lighting infrastructure, and simulation parallelism.

### Shippable exit and gates

An internal packaged simulation laboratory with reproducible seeds, scripted interactions, overlays, benchmark CSVs, and checkpoint round-trips.

Must demonstrate:

- Finite temperatures, non-negative mass, explicit source/sink accounting.
- Cross-chunk conservation, deterministic traversal, wake propagation, and no starvation.
- Measured 60 Hz feasibility on the selected minimum machine.
- Full-grid and checkpoint peak-memory ceilings.
- Replay equality and v4 migration-or-rejection fixtures.
- Fluid-enabled time trials and `load100` without growth or drift.

A dirty-delta capture alone does **not** qualify as a standalone save. Legacy v4 deltas require their valid reconstruction context; missing baseline information must not be invented.

### Decide at this gate

- Minimum hardware, platform/backend matrix, memory ceilings, supported simulation speeds.
- Dense-grid viability and maximum supported activity envelope.
- Conservation tolerances, numerical stability limits, and solver-latency limits.
- Meaning of flag combinations and phase invariants.
- Save-version policy, stable content IDs, and intended commercial scope.

### Remains open

Balance values, biome counts, final art direction, recipe quantities, and advanced visual effects.

**Top risk:** bounded work hides an inadequate solver.
**Mitigation:** gate on simulation progress and conservation as well as tick duration. If infeasible, stop and explicitly approve a substrate or scope change before content production.

---

## 4. MVP-1 — Breathe

### Entry

P0 passes; the substrate, authoritative schedule, and checkpoint format are proven.

### IN — prioritized implementation

1. **Single-avatar platformer:** movement, jumping, ladders, collision, camera, pause, keyboard aiming.
2. **Tools and construction:** dig, vacuum, inventory, storage, resource costs, staged commit/cancel, invalid-placement feedback.
3. **Survival:** oxygen, stamina, warmth, bladder baseline, food accounting, centralized hazard damage.
4. **Closed early economy:** hand-wheel → battery → hub → consumers; electrolysis and finite resource inputs.
5. **Research T0–T1 and real farm/cook baseline.** Farming must replenish consumed calories; starting food is not loop closure.
6. Placeholder imperative chain with explicit completion/failure and recovery/restart.
7. Base-pack schema validation/code generation, stable IDs, hashes.
8. Local tick-stamped intent log, validation receipts, replay.
9. Named save slots, autosave ring, transactional load, asynchronous file writing from immutable captured state.
10. Essential HUD, overlays, rate-based tooltips, severity bubbles, and keyboard-completable build flows.

Before full plumbing ships, use a real bounded resource-transfer interaction for water-consuming buildings—not hidden infinite water or invented mass.

### DEFERRED

Full liquid networks, complete phase/fire interactions, broad biome content, final missions, complete audio/VFX, and all network/DLC features.

### Shippable exit and gates

A standalone vertical slice in which a player can:

> Land → build shelter → generate/store/distribute power → produce oxygen → explore/research → farm/cook sustainably → complete the placeholder imperative → save, quit, reload, and continue.

Additional gates:

- Scripted and human-tested completion without developer intervention.
- Gas/thermal workloads in time trials and `load100`.
- Local replay and save-continuation equality through building, consuming, and destroying resources.
- Save failure tests: disk full, access denied, cancellation, interrupted replacement, and corrupt slot.
- Async save teardown drains/cancels safely; failed saving preserves the previous valid slot.

### Decide at this gate

- Core movement/tool feel.
- Early progression and sustainable resource balance.
- Death/recovery model.
- Save-slot UX and support policy for player-facing builds.
- Whether the loop is enjoyable enough to justify content expansion.

### Remains open

Late-game economy, advanced automation recipes, final mission writing, content volume.

**Top risks:** weak platforming; oxygen/food loop softlocks; persistence added too late.
**Mitigation:** movement playtests first, resource-ledger tests, early usable saves, and explicit recoverable failure states.

---

## 5. MVP-2 — Plumb & Burn: complete mechanical MVP

### Entry

Breathe is independently playable, sustainable, replayable, and saveable.

### IN — prioritized implementation

1. Production liquid flow and gas/liquid displacement.
2. Thermal advection/conduction, phase transitions, hysteresis, reactions, and fire.
3. Non-grid pipes, pumps, vents, filters, capacities, throughput, and blocked-output behavior.
4. Power circuits, storage, brownout allocation, generators/turbines.
5. One-bit automation with defined observation/application timing.
6. Cycle clock and all specified 1 Hz/10 Hz divisors.
7. Integrated overlays, network inspection, construction previews, fault explanations.
8. Minimum implementations covering the remaining inventory: representative biomes, geyser, ambient critter, lubricant progression gate, and imperative quotas.
9. Sufficient research/farm/cook progression to exercise the complete coupled economy.

### DEFERRED

Production breadth and final presentation until Beta. Mixture cells, secondary payloads, ribbons, scheduling UI, scripting/mods, networking, and DLC remain outside this delivery.

### Shippable exit and gates

A small but complete survival-simulation game: the player can build a sustainable installation, encounter and recover from infrastructure hazards, and complete the scoped MVP objective chain.

Required tests include:

- Mass and energy accounting across **world cells ↔ pipes ↔ buildings ↔ inventory**.
- Blocked pipes, conflicting automation, brownouts, and demolished networks.
- Phase-boundary oscillation, fire propagation, cross-chunk flow, and inactive-neighbor wake.
- Exactly one damage application path.
- Coupled fluid time trials and `load100`.
- Proposed minimum **8-hour automated coupled-system soak**.
- Checkpoint/load during active reactions and network reconstruction.

### Decide at this gate

- Mechanical feature lock for the base game.
- Final physical abstractions and failure semantics.
- Supported activity/speed limits.
- Production content list, quantities, campaign length, and Beta acceptance matrix.
- Whether lighting is gameplay-required. If required, fund a bounded implementation for Beta; otherwise keep propagation deferred.

### Remains open

Exact balance, content arrangement, art/audio polish, optional nonessential effects.

**Top risk:** individually correct systems produce unstable or exploitable coupled behavior.
**Mitigation:** integrated resource-ledger tests and adversarial scenarios, not only isolated kernel tests.

---

## 6. Beta — content complete

### Entry

Mechanical MVP passes; the full base-game content inventory and campaign scope are approved.

### IN

- All launch biomes, generation rules, traversal/resource guarantees, and environmental hazards.
- Geyser variants, periods/dormancy, and progression integration.
- Ambient critter species, behavior, animations, and approved drops—**not ranching**.
- Complete farm/cook recipes, environmental requirements, food economy, and storage behavior.
- Full research progression, construction catalogue, turbine/lubricant tuning.
- Final imperative chains, objectives, rewards, failure states, and ending.
- Production avatar/building/tile art and animation.
- Audio integration: device lifecycle, bounded voice use, buses/settings, biome ambience, loops, and gameplay/UI feedback.
- All required VFX with bounded lifetimes and quality controls.
- Production save/load UX, versioned compatibility fixtures, autosave behavior, and recovery messaging.
- Localizable text and asset pipeline; no new hardcoded player-facing text.

Audio and VFX consume presentation events; they must not mutate simulation state. Saved state reconstructs ongoing loops without replaying obsolete one-shot events.

### DEFERRED

New mechanical families, new campaign scope, speculative renderer features, DLC, and networking.

### Shippable exit and gates

A feature/content-complete closed Beta: all promised base-game content is present, with no placeholder objectives or progression blockers.

Required:

- Fresh-run completion across a curated seed set and broader automated generation corpus.
- Every research node, recipe, biome, and objective has an acceptance test or recorded playtest.
- Complete fluid benchmarks with production assets and saves enabled.
- Proposed **24-hour automated soak** and repeated save/load/restart cycles.
- Agreed pre-Beta save fixtures migrate successfully; incompatible builds fail clearly.
- No known critical progression blockers, resource softlocks, or save corruption.
- Originality/licensing review complete for shipping assets and names.

### Decide at this gate

- Content freeze.
- Public save-compatibility baseline.
- Final language/platform lists.
- Final art/audio quality targets and remaining defect priorities.

### Remains open

Balance adjustments, onboarding revisions, accessibility refinements, and targeted performance fixes.

**Top risks:** content explosion, unreachable progression, and unsupported prototype assets.
**Mitigation:** fixed content ledger, seed/progression validation, asset provenance tracking, and early external playtests.

---

## 7. Polish — usability, accessibility, performance

### Entry

Beta content is complete; remaining work is improvement and defect removal, not missing systems.

### IN

- Onboarding that teaches movement, staged construction, oxygen, heat, plumbing, power, and sustainable food through play.
- Keyboard-only completion; rebinding, focus/navigation, readable UI scaling, and supported display configurations.
- Non-color-only overlays, legends, severity hierarchy, rate-based explanations.
- Audio-independent critical feedback; motion/flash controls and appropriate subtitle support.
- Save warnings, destructive-action confirmation, objective clarity, and error recovery.
- Performance profiling on minimum hardware, bounded caches/pools, upload reduction, checkpoint capture optimization.
- Only evidence-backed concurrency changes.
- Consistent animation, effects, mixing, and interaction feedback.

### DEFERRED

New gameplay, additional biomes, expanded campaign, new platforms, networking, and DLC.

### Shippable exit and gates

Release candidate that new players can learn without developer assistance.

- Predeclared onboarding completion and failure-rate targets pass external testing.
- Full critical path is keyboard-completable.
- Accessibility audit passes the committed feature list.
- Worst supported content/simulation workload meets frozen budgets.
- Proposed **72-hour soak**, including autosaves, device events, repeated menus, and teardown.
- Replay/save fixtures remain green after optimizations.
- Save peak memory and frame stalls meet explicit limits—not merely average throughput.

### Decide at this gate

- Final minimum/recommended requirements.
- Release settings and default difficulty.
- Accessibility claims.
- RC acceptance criteria and bug-severity release blockers.

### Remains open

Only low-risk tuning, cosmetic fixes, and release-blocker remediation.

**Top risk:** optimization or “small improvements” destabilize finished gameplay.
**Mitigation:** feature freeze, replay comparisons, targeted profiling, and rejecting speculative rewrites.

---

## 8. Gold — commercial release readiness

### Entry

Polish RC meets gameplay, UX, performance, and stability targets.

### IN

- Final regression, installer/depot/package validation, and clean-machine testing.
- Localization integration and linguistic QA.
- Required regional ratings, store disclosures, legal notices, asset licenses.
- Achievements through an isolated platform adapter; gameplay never depends on platform availability.
- Store packaging, offline behavior, supported account/device cases.
- Crash diagnostics, private symbol retention, support documentation, patch/rollback rehearsal.
- Definitive save-upgrade and backup policy.
- Final store assets, controls documentation, credits, and accessibility information.

Cloud saves, if commercially promised, need a separately tested conflict policy; they are not implied by ordinary local saves.

### DEFERRED

DLC packs, commerce/entitlement machinery, multiplayer, lobbies, transport, prediction, and all cut gameplay systems.

### Shippable exit and gates

- Exact signed/staged release artifact passes installation, launch, play, save/load, update, and uninstall tests.
- No open release-blocking crashes, data-loss defects, or progression blockers.
- Full compatibility corpus passes; updates cannot silently discard incompatible content.
- Final minimum-spec performance and soak results attached to the release.
- Localization, achievements, ratings, licenses, and store requirements approved.
- Patch deployment and rollback tested, including save-version consequences.

### Decide at this gate

Release go/no-go, supported versions/platforms, support ownership, and post-launch maintenance commitments.

### Remains open

Future expansion content and optional co-op—not unfinished base-game requirements.

**Top risk:** a good development build fails as a distributed product.
**Mitigation:** validate the actual depot/package on clean machines, not only developer environments.

---

## 9. DLC and co-op seams: specify now, do not build now

“No rewrites later” should mean **no replacement of the core ownership, authority, or persistence model**. It cannot honestly mean that future multiplayer needs no gameplay or UI changes.

| Seam | Base-game requirement | Explicitly not built |
|---|---|---|
| Pack manifest + hash | Versioned base manifest; stable qualified content identity; deterministic runtime-ID mapping; schema/reference validation; saves record exact identity/hash | DLC downloader, entitlements, marketplace, arbitrary runtime plugins, hot mounting |
| Intent boundary/log | Tick-stamped actor ID + sequence + typed payload; authoritative validation and receipts; local replay; UI cannot write sim state directly | Sockets, lobby, remote authentication, bandwidth protocol, prediction |
| Snapshot ancestry | Full baseline identity/tick; ordered delta parent/base identity; deletions; full-state checksum; transactional restore | ACK/retransmit ring, reconnect service, network resync implementation |
| Overlay slots | Stable overlay identifiers, explicit ordering, view-data providers, independent presentation invalidation | Empty DLC overlays, reserved gameplay components, speculative render passes |

Additional safeguards:

- Ship one locally controlled actor; avoid implicit global “the player” dependencies in intents, ownership, and save identity.
- Keep runtime content IDs stable through explicit mapping; never derive persistent identity from file enumeration order.
- Unknown/missing pack content produces an actionable compatibility error, not silent substitution.
- Future co-op remains **host-state-sync only**. Do not pay for lockstep determinism now.
- Do not widen or replace the 64-component ECS preemptively. Track component usage; use IDs and external definition tables for variable content.

## Final priority

**Prove solver and persistence → prove the complete survival loop → complete coupled mechanics → finish content → polish usability/performance → certify the commercial artifact.**

Do not approve a gate because an isolated demo looks convincing. Approve it because the packaged phase is playable, measurable, replayable, saveable, and demonstrably within its supported limits.
