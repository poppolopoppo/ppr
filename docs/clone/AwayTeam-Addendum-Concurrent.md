# Addendum: concurrent execution and responsiveness `[DECISION]`

**Applies to:** `docs/clone/AwayTeam-Addendum-Async.md`; `AwayTeam-Architecture.md` §4.2, §5–6, §8–9; `AwayTeam-Roadmap-To-Gold.md` P0/MVP-2/Polish; `AwayTeam-UX.md` §2.1/§11; `AwayTeam-Gameplay-Inventory.md` §4.3.

## 1. Decision and scope

**Input/update, authoritative simulation, and render preparation/submission run concurrently on separate threads.** Event polling must continue during solver execution, GPU waits, and saving.

“Single-threaded correctness first” means retaining one authoritative simulation owner and a serial reference implementation—not keeping the application’s entire frame loop serialized. Solver workers are introduced only where measurements justify them.

This clarifies Roadmap §1/§3 and Addendum-Async’s “retain single-threaded execution initially”: defer **parallel solver kernels**, not the required input/sim/render separation.

Preserve the four-field, single-element Cell; S00–S16 semantics; 60 Hz simulation; integer 1 Hz/10 Hz divisors; and renderer-independent `engine.sim`. Networking remains deferred host-state-sync, not lockstep.

## 2. Thread topology and ownership

| Execution lane | Exclusive mutable ownership | Boundary traffic |
|---|---|---|
| **Platform/input/update thread**—main thread where platform requires | Event polling, input arbitration, tool gestures, camera, UI context and previews | Tick-intent requests; immutable camera/UI packets; simulation receipts/read models |
| **Simulation thread** | `ColonyDriver`, authoritative grid/ECS, activity masks, cursors, RNG, game state; normally physics scene | Sealed tick inputs; immutable render/checkpoint captures; bounded worker jobs and results |
| **Render-submit thread** | `ColonyTranslator`, pass caches/resources, renderer and GPU submission | Immutable simulation presentation packets, `CameraSnapshot`, owned UI draw data |
| **Bounded worker pool** | Job-local scratch and outputs | Immutable kernel inputs/halos; indexed result records |
| **I/O/compression lane** | Save requests, compression buffers, file operations | Immutable checkpoint captures; completion/error receipts |

Workers never access mutable live ECS or grid storage. The existing SoA ECS remains single-threaded; component views and entity references do not cross mutation boundaries.

Keep ImGui confined to its owner. Rendering receives owned draw data, not pointers into next-frame UI storage. `ColonyTranslator` remains render-thread-confined; workers may prepare plain data, not call its mutable staging APIs.

`Renderer` remains content-free and camera-free; passes receive `CameraSnapshot`. Concurrency does not expand GridPass beyond its current tint substrate or remove TrianglePass’s fixed-key/blend restrictions.

**References:** Architecture §4.2/§5; UX §2.1/§11.

## 3. Tick pipeline and bounded presentation

While simulation computes tick **N**, rendering prepares/submits the latest complete presentation, normally **N−1**, and input independently captures new events.

- Seal each tick’s accepted intent set before its first dependent phase. Record the authoritative tick and stable sequence; late input waits for the next tick.
- Preserve the canonical schedule, locomotion ordering and previous-tick power semantics. Do not overlap dependent authoritative ticks.
- Publish complete presentation packets tagged with world generation and tick. Publish interpolation timing/`alpha()` coherently; rendering must not read the mutable tick driver.
- Interpolate presentation only between valid samples. When simulation falls behind, retain the last valid state rather than extrapolating authoritative outcomes.
- Use two bounded staging slots with explicit producer ownership, release/acquire publication, consumer ownership and reclamation. A slot is reusable only when CPU borrowing and any GPU use have ended.
- If both slots are busy, skip publication and coalesce pending presentation changes. Continue simulation and input; retain the last valid rendered frame. Never overwrite an in-use slot.
- Preserve accumulated dirty regions/deletions until represented in an acknowledged handoff. Replacing a pending delta requires merging its changes or rebuilding against the consumer’s known version.
- Double buffering means bounded presentation staging—not two additional 4096² world copies. Keep `SimActivity` separate from presentation dirty state.

Overlay, camera and UI updates must remain responsive even without a new simulation packet. Preview acceptance remains provisional until the simulation receipt arrives.

**References:** Architecture §4.2/§5; Addendum-Async, “Double-buffer render staging”; UX §2.1/§11.

## 4. Deterministic worker execution

First implement the same partition/merge algorithm serially; workers change execution placement, not numerical semantics.

1. Select work using fixed work-count budgets and persistent carry-over cursors.
2. Freeze each phase’s required inputs and halos after preceding dependencies commit.
3. Assign every cross-partition face exactly once.
4. Produce worker-owned transfer proposals; workers do not mutate neighboring cells or authoritative ECS.
5. Merge in fixed partition/face order, with deterministic arithmetic and aggregate donor limits across **all** outgoing proposals.
6. Commit activity/wake changes without modifying the active sweep.

Worker count, completion order and wall-clock deadlines must not choose which results count. Late required results delay the dependent phase, not input handling. Save replay-relevant scheduling state.

**Physics:** initially step the driver-owned Box2D scene serially. If off-thread stepping becomes necessary, give the entire step exclusive scene access and join before dependent phases or capture. Do not manually split a world by grid chunk: contacts/joints cross boundaries. Island parallelism requires supported backend tasking, dependency barriers, deterministic result publication and continuation tests; collider edits, queries and body destruction cannot race the step.

**References:** Architecture §4.2/§9; Inventory §4.3; Addendum-Async, solver and scheduling rules.

## 5. Channels, memory and overload

Use `engine.core` bounded channels, `Signal/select` and `IContext`; do not introduce a general-purpose scheduling framework.

- Each RawChannel has exactly one consumer; distribute worker jobs accordingly. MPSC does not imply multiple-consumer safety or wait-free producer latency.
- Specify capacity, maximum age, full/empty/closed behavior and cancellation for every boundary.
- Coalesce replaceable state such as camera position. Never silently discard discrete commands, accepted authoritative changes or save results. Saturated command admission returns visible busy/rejected status without blocking event polling.
- Use owned, bounded captures and worker-local arenas. No cross-thread borrowed scratch; `ScratchPad`’s 32 KiB ceiling excludes large halo/staging payloads.
- `safe_ptr` is neither ownership nor synchronization; debug lifetime checking is not a release-build safety mechanism.

**References:** Architecture §8–9; Roadmap §2; Addendum-Async, “Bound everything.”

## 6. Checkpoints, cancellation and teardown

Capture a coherent immutable checkpoint at a defined completed-tick boundary, after required worker/physics commits. Compression, encoding where safely separable, checksums and filesystem I/O consume only that capture off-thread.

Preserve v5 baseline-versus-delta ancestry, transactional restore and complete continuation state. Bound capture memory and queued saves; measure capture stalls separately from write latency. Publish save success only after the required durable replacement completes. Loading invalidates old-generation presentation and worker results.

Teardown order:

1. Disable callbacks and new submissions.
2. Cancel child `IContext`s and explicitly wake blocked lanes.
3. Drain or cancel accepted work; acknowledge I/O cancellation and join threads.
4. Retire GPU work, then release captures, staging, channels and resources.

Cancellation alone is not completion or a guaranteed wakeup. Keep consumers alive while draining producers; attempt independent cleanup and return the first error.

**References:** Architecture §6/§9; Addendum-Async, persistence and bounded-lifetime rules.

## 7. Delivery and acceptance gates

- **P0:** establish serial canonical results, then demonstrate concurrent input/sim/render with bounded handoffs. Ratify minimum hardware, input-to-feedback and input-to-commit limits, capture-stall limits, queue capacities, sweep-time and RSS ceilings. Solver parallelism remains measurement-gated.
- **MVP-2:** scale only measured bottlenecks. Test large entity/event counts, widespread offscreen activity, cross-partition flows, active saving and render saturation. Require conservation, no starvation and bounded presentation/command age.
- **Polish:** optimize measured hotspots only; verify shutdown, overload recovery, device events and long soaks on minimum hardware.

Record input latency, tick/frame p95/p99, worker waits, oldest queue age, complete-sweep time and peak RSS. Roadmap’s proposed tick p95 ≤8 ms/p99 ≤12 ms remain **targets to ratify**, not measured facts.

Every concurrency change requires measurable improvement without input-latency regression, plus same-build replay and checkpoint-continuation equality across worker counts and delayed/reordered completion tests. A mode switch that changes canonical results is an explicit compatibility change—not a transparent optimization.
