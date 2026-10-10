# Refined frame-refactor plan (verbatim oracle output, ses_ef3a162ebffe7bJJD08AYuMTt8)

> **Document scope:** Design/proposal record, not a current implementation inventory or repository-wide instruction. Apply its task-specific requirements only when the active task explicitly adopts this design. Verify implementation claims against source/CMake and the current [documentation guide](../README.md); historical paths, counts and phase status may be stale.

**Recommendation:** retain P0–P2 and the two-credit baseline. Make P3 specifically **platform/update isolation**, justified by measured latency—not “optional simulation parallelism.” The accepted ~0.25 ms simulation cost does not justify another lane by itself.

The draft is directionally sound but its ownership, admission, presentation, and failure contracts need the changes below before implementation.

Read-only review completed. No files changed or validation executed; **the orchestrator owns validation**.

## Changes required—and why

| Draft proposition | Replacement |
|---|---|
| “Two credits” without an exact lifetime | One credit covers a frame from construction through retirement. Building, queued, recording, and GPU-pending frames collectively consume at most two credits. |
| Main thread blocks acquiring a credit | Admission is nonblocking from the platform event loop. No credit means keep servicing platform events/control and wait interruptibly—not block the pump. |
| Render threading achieved by moving existing methods | First separate lane-agnostic stages and owned inputs. A topology adapter chooses inline execution or a dedicated lane. |
| `CameraSnapshot` as a borrowed const-reference | Copy the snapshot into the owned packet; pass a const-reference only while consuming that packet. |
| Submit fence means everything is finished | Separate GPU completion, host present-call completion, and surface/backbuffer lifetime. A graphics fence is not proof of display completion. |
| Dropping an obsolete packet is automatically safe | Dropping presentation must not discard unacknowledged world edits, resource changes, or translator deltas. |
| Two/three ring slots establish GPU safety | Every mutable resource requires an explicit last-use completion rule. Slot counts are capacity, not synchronization. |
| Cancellation guarantees bounded shutdown | Cancellation wakes engine waits; it does not interrupt an arbitrary driver `present()` or GPU wait. State the supported failure/escalation policy. |
| Four review gates with bounded attempts imply eventual approval | Exhausting attempts stops the phase. It never waives unresolved safety findings. |

### Gallery corrections are part of plan approval

The current diagrams contradict the intended architecture:

- `03-final-topology.puml:12`: blocking platform/client credit acquisition is wrong.
- `03:18`: atlas preparation belongs to the render-owned stage, not the client.
- `03:39–43`: simultaneously building N+1, recording N, and executing N−1 requires **three live frame credits** under the proposed retirement contract. Two credits permit pairwise overlap, not three distinct in-flight frames.
- `04-memory-ownership.puml:14`: replace the borrowed camera snapshot with packet-owned data.
- `04:37–46`: generation rejection is safe before acquisition; after acquisition, the image still needs the backend’s required release/present handling. Publication safety is release/acquire, not merely the producer mutex.
- `02-refactor-phases.puml:30–33`: smooth P2 performance is a reason to **defer** P3, not enter it.

---

## Architecture contract shared by all phases

### 1. Small stages; explicit placement

Use named operations with explicit inputs and results:

1. **Service platform:** collect native events, device samples, window state, and commands.
2. **Advance client:** apply input, run timers/client logic/simulation, update camera, construct UI.
3. **Build packet:** produce owned immutable render inputs.
4. **Prepare render:** translate inputs, update pass-local resources, prepare uploads.
5. **Execute:** acquire, encode with the existing single encoder, submit, present.
6. **Retire:** observe completion, release retained resources, return credits.

These are responsibilities, **not six new interfaces or a general task graph**. Keep ordinary functions and a small frame coordinator. Use the same stages in inline and threaded configurations; do not maintain two render implementations.

Thread placement must not change ownership contracts or introduce hidden service resolution.

### 2. Ownership and affinity

| State/resource | Owner and exclusive execution lane in P2 |
|---|---|
| Application lifecycle, pacer, frame-slot storage, transport | Application-owned coordinator; explicit producer/consumer roles |
| GLFW, native windows, raw input sampling, platform commands | Platform/main |
| Scene, player, input routing, camera/controller, viewport model, simulation | Client/main |
| ImGui context, input integration, widgets, `NewFrame`/`Render` | Client/main |
| Detached UI render data | Frame slot, immutable after publication |
| `ColonyTranslator` and its cache | Game-owned render component; render lane |
| Triangle/Grid/UI GPU state and pass caches | Client/editor-owned render components; exclusively render lane |
| Renderer, surfaces, queue, encoding and presentation | Application-owned Renderer; exclusively render lane |
| Frame CPU backing storage | Stable Application-owned slots; reset/destruction on the designated owner lane |
| GPU resources referenced by a submission | Retained until their actual last-use completion |

Ownership and execution affinity are different: running an editor-owned pass on the render lane does **not** transfer scene ownership into Renderer.

`safe_ptr` remains an observer, never synchronization or shared ownership. Do not pass mutable `Window`, camera, driver, service stores, or live ImGui objects across lanes.

Keep game snapshots/translation in `game`; do not introduce `engine.app → engine.sim/physics`. Renderer boundary types remain camera-free. Camera snapshots belong to pass inputs; viewport/scissor remain per draw.

### 3. Frame-slot protocol

Use stable slots and move-only leases/handles, not arbitrary callbacks capturing application objects.

```text
Free → Building → Published → Recording → Submitted → Retired → Free
```

Define the exceptional transitions explicitly:

- Build/publication failure: `Building → Retired`.
- Rejected obsolete packet before acquisition: `Published/Recording → Retired`.
- Failure after acquisition but before submission: release the acquired image through the backend-required path, then retire.
- Successful submission followed by present failure: remain GPU-pending until completion or a documented device-loss teardown path.
- Submission accepted but completion tracking failed: quarantine; never invent completion or immediately recycle.

**Exactly one terminal retirement and one credit return per admitted frame.**

Publication contract:

- Producer finishes packet writes before release publication.
- Consumer acquire establishes visibility and exclusive consumption.
- Consumer release-publishes retirement; producer acquire precedes destruction/reset/reuse.
- Existing channel synchronization may supply these edges. Avoid a second competing atomic ownership protocol.
- Queue-record release is **not** frame retirement.

Begin with conservative packet/storage retention through retirement. Earlier CPU reclamation can be a separately proven optimization.

### 4. Packet contents and resource lifetime

Packets contain values and owned storage:

- Frame ID, slot generation, world epoch, surface identity/generation.
- Latched frame timing and completed-input-batch sequence.
- Copied `CameraSnapshot`, viewport/scissor, render descriptions.
- Owned colony/chunk/debug-instance inputs.
- Detached UI vertices, indices, commands, display metadata and valid resource identities.

No producer `ScratchPad`, borrowed upload spans, initializer-list backing, or `function_ref` may escape into transport. Existing `DrawSubmission` callbacks and temporary pass descriptors can still be constructed **on the render lane for synchronous encoding**.

Prefer provisioned GPA/OS-backed slot storage and explicit owners initially. Do not transfer TLS allocation handles. If a slot arena is used, its allocator lifetime, single-user access, object destruction and reset lane must be explicit; arena reset does not run destructors.

Capacity exhaustion needs a visible error or a documented bounded fallback—not accidental `Pooling` termination or an assumed recoverable arena failure.

### 5. Reliable state versus disposable presentation

Initially use bounded FIFO admission; do not introduce “latest frame wins” casually.

If stale surface frames are skipped:

- Reliable resource/world changes must still be applied, replayed, or represented in a subsequent self-contained snapshot.
- Dirty chunk acknowledgement occurs only at the defined consumption point.
- Regeneration carries an epoch that invalidates older translator state.
- Render statistics return as owned, frame-labelled messages; the client must not read live translator/pass caches for its panel.

For UI, deep-copying draw lists alone is insufficient: retain texture/resource identities and explicitly handle callbacks. Support known render-only commands such as reset-render-state; reject unsupported client-context callbacks rather than invoking them on the worker.

---

## P0 — Serial-explicit stages, pacing and lifecycle correctness

**Execution:** one CPU lane, one frame credit.
**Owner:** Application coordinator; existing client/game owners remain unchanged.

### Work

1. Extract the stages while preserving the meaningful existing ordering:
   - `Application::run`: time advance/callbacks → client update → render.
   - GLFW device polling before event pumping.
   - Editor viewport → camera → pass inputs/UI production.
   - TurboLarbin regeneration, input/error handling, fixed-step driver, edit presentation, hover/debug work and UI phases.
   - ColonyDriver’s 60 Hz accumulator, five-slice bound and remainder policy.

2. Introduce owned packet construction and render consumption **on the same thread first**. This is where borrowed simulation, camera and UI dependencies become visible.

3. Extract pacing from `TimerManager::tick`; call it with a zero target period.
   - The pacer receives an explicit monotonic clock and wait mechanism.
   - Initially preserve the existing minimum-period policy; do not silently change to phase-locked scheduling or catch-up bursts.
   - `TimerManager::now()` remains latched by `tick`, not a fresh-clock accessor.
   - Waiting must not depend on a deadline callback from the same TimerManager that cannot tick until the wait ends.
   - Preserve callback timing/order and `dt`/elapsed semantics with fake-clock tests.
   - Focus/background/uncapped transitions explicitly re-evaluate admission.

4. Separate native event collection from once-per-client-frame input advancement.
   - Additional pumps while waiting must not repeatedly clear transient input, advance device time, or lose press/release pairs.
   - Completed-pump batches provide a clean boundary; client dispatch occurs at the defined update point.
   - Record this as an intentional responsiveness change, rather than claiming every callback timing remains identical.

5. Coalesce/rate-limit titles **only where a live title producer exists**. Do not reintroduce the stale codemap’s removed Editor rename operation.

6. Fuse lifecycle completion:
   - Retain the first operational error through all cleanup.
   - Finish shutdown and logger drain before choosing `run()`’s return value.
   - Do not depend on a lifecycle object that shutdown resets.
   - Make graceful exit wake waits without converting it into cancellation.

This addresses a concrete current issue: `App.Application.cpp:92–104` performs cleanup in a defer, while `:153` evaluates the return beforehand. Cleanup errors cannot reliably become the returned result under that shape.

### Gate 0

Require:

- Serial phase-order trace and deterministic game-state equivalence.
- Packet ownership proof and allocator failure behavior.
- Fake-clock coverage: early/late/overrun, uncapped, focus changes, cancellation, timer callback errors.
- Graceful exit while waiting, partial initialization and cleanup-error propagation.
- Input edge preservation across multiple pumps per client frame.
- Headless/no-render/no-UI domain paths still valid.

**Exit criterion:** inline stage execution is the reference implementation; no concurrency-dependent correctness remains hidden in client callbacks.

---

## P1 — Fence-governed reuse, still one CPU lane

**Execution:** same thread; two frame credits, bounded asynchronous GPU work.
**Owner:** Renderer owns submission/completion tracking; each pass owns its resources.

### Work

1. Establish the backend contract before implementing retirement:
   - Fence signal placement and completion query/wait.
   - Queue ordering and submission failure semantics.
   - Acquired-image obligations on encode/submit failure.
   - Presentation and surface destruction requirements.

2. Signal a monotonic completion value after the frame’s actual GPU work. Associate resources with **submission completion**, not CPU frame modulo or swapchain image count.

3. Produce a resource-reuse table covering:
   - Grid/Triangle payload rings and indirect arguments.
   - UI `FrameResources[2]`, growth and replacement.
   - Constants, descriptors, caches, pipelines and textures.
   - Atlas writes and cache eviction.
   - Depth attachments, surface reconfiguration and offscreen readback.

4. Keep GridPass’s existing safe atlas fence as the first implementation.
   - Replace global-idle waiting with last-reader tracking only if proven correct.
   - Versioning/duplicating the large atlas is not a prerequisite.
   - If the existing atlas wait serializes dirty frames, expose that in profiling; do not claim overlap that does not occur.

5. Add generational surface state and lifetime acknowledgements.
   - Metadata is copied; native-window lifetime is retained by an explicit platform/render handshake.
   - Zero-size/minimized surfaces are an explicit non-rendering outcome.
   - Reject obsolete work before acquisition.
   - After acquisition, complete the required backend obligation.
   - Resize/close control must be serviceable without obtaining a frame credit.

6. Retirement requires completion of relevant host operations and GPU uses. Swapchain/display ownership remains governed by its own backend contract.

### Gate 1

Require:

- Artificially delayed completion proves no early overwrite/free.
- Two outstanding GPU submissions demonstrated on an eligible workload—not merely two allocated slots.
- Upload growth, atlas edits, regeneration, pipeline/cache replacement, resize/DPI/minimize/restore tested.
- Acquire/encode/submit/present failures settle every lease correctly.
- GPU validation/readback and shutdown-under-load evidence.
- A distinct measurement for queue depth, host present duration, GPU completion and displayed cadence where available.

**Exit criterion:** every mutable GPU resource has a documented and tested reuse rule. Adding a worker is then a CPU ownership change, not a GPU-lifetime gamble.

---

## P2 — Dedicated render lane

**Execution:** platform/client main lane + render lane; exactly two frame credits.
**Owner:** Application owns the dedicated worker; content owners supply their render components.

### Work

1. Move render preparation, translator execution, pass GPU work, Renderer calls and corresponding lifecycle operations to one fixed lane.
   - Preserve Renderer’s affinity checks.
   - Resolve/inject services at startup boundaries.
   - Do not “migrate” an already active Renderer by changing its owner ID.
   - Keep backend-required platform operations on main.

2. Split ImGui frontend from GPU backend.
   - Client completes `ImGui::Render()` and publishes detached data.
   - Worker never reads the current context, live IO, `GetDrawData()`, or mutable frontend backend fields.
   - Font/texture creation and replacement use explicit ownership/acknowledgement.

3. Introduce bounded transports:
   - One logical consumer per `RawChannel`.
   - Frame handles forward; completion/status back.
   - Explicit control/wake path independent of frame capacity.
   - Use nonblocking admission on the platform path.
   - Full/empty/closed/cancelled/error outcomes remain distinguishable.

   Do not assume `wait_if_full`, `flush`, or terminal `close` is cancellable. A full queue must not prevent exit signalling; a failed worker must not strand the producer in channel cleanup.

4. Integrate the pacer with the platform event loop.
   - Admission requires deadline eligibility **and** a free slot.
   - While ineligible, service native events/control and wait on a wakeable platform-compatible mechanism.
   - Credit return, exit and relevant configuration changes wake it.
   - Notifications are hints; recheck persistent exit/error/credit state before sleeping.
   - Two credits bound throughput backlog, not latency during an unbounded driver stall.

5. Preserve derived-application behavior through explicit hooks.
   - Do not move arbitrary existing virtual `render()` bodies wholesale to another thread.
   - Adapt Editor/TurboLarbin and smoke/offscreen hosts deliberately.
   - Retain inline execution for reference/testing without duplicating stage logic.

### Teardown protocol

1. Stop admission and detach client/game callbacks that can enqueue work.
2. Signal exit/cancellation and wake all engine waits.
3. Settle admitted packets: discard unsubmitted work safely; retire submitted work.
4. Keep required platform servicing alive while the render lane quiesces. Do not block main in `join()` if the worker/backend needs the pump.
5. On render lane, release pass GPU resources, surfaces, Renderer and dependent services in valid reverse order.
6. Acknowledge native-window release; destroy windows/platform state on main.
7. Join/quiesce emitters before destroying subscriptions, channels, contexts or slot storage.
8. Return the first error after attempting independent cleanup.

Never detach a stuck worker and destroy its dependencies. Driver-hang handling needs a documented watchdog/process-level policy; it is not solved by `CancelContext`.

### Gate 2

Require:

- Affinity assertions and exclusive-owner inventory.
- Deterministic tests for full queues, publication/retirement races, worker failure and graceful exit.
- Detached UI lifetime/callback/texture tests.
- No cross-lane service-store, live window, driver, camera, or pass-cache access.
- Sanitizer/debug-allocator checks where supported, plus backend GPU validation.
- Reproducible PIX comparison against P0/P1 using the same workload and presentation settings.

**Exit criterion:** genuine client/render overlap without increased unbounded backlog, input loss, stale-resource use or teardown regressions.

---

## P3 — Conditional platform/update isolation

**Default disposition: defer.** This is not an automatic continuation of P2.

### Entry criteria—all required

1. **P2 passes correctness gates.**
2. **Numeric SLOs are agreed before the experiment:** update lateness, input age, packet age and display interval at named target rates.
3. Representative repeated captures show p95/p99—and sufficiently sampled p99.9—violations attributable to platform/client coupling.
4. Pump tails persist after applicable title/coalescing fixes.
5. Evidence distinguishes:
   - One slow `PeekMessageW` from a long queue drain.
   - Running, waiting and ready time.
   - Platform delay from GPU/present bottlenecks.
6. Input-age and packet-age budgets show that isolating update can help despite stale input during a blocked pump.
7. There is a concrete target workload and measurable acceptance threshold; thread count alone is not a success criterion.

The accepted ~12 ms tail adds roughly 0.72 normal 60 Hz steps. It does not, by itself, establish five-step catch-up failure or justify a sim worker.

### Topology and ownership

- **Platform/main:** native windows, raw callbacks/device sampling and asynchronous platform commands.
- **Update/client:** TimerManager, input routing/player/controller, simulation, camera, UI frontend and packet construction.
- **Render:** unchanged from P2.

All ImGui/input-routing logic stays with its owning client lane. Platform callbacks publish raw owned events; they do not invoke update-owned listeners directly.

### Required protocol

- Sequenced completed-pump batches with sampling/completion timestamps and window/focus/device generations.
- No consumption of partially collected batches.
- Bounded input storage: coalesce appropriate motion/analog state, preserve ordered edges/text/control, report overflow explicitly.
- Update-owned clock and schedule; never drive its `dt` from pump duration.
- Explicit stale-input policy: held-state age, focus/disconnect reset, gamepad freshness, timeout behavior.
- No invented release edges or replayed one-shot actions.
- Platform commands carry owned arguments and acknowledgements where required; no synchronous callback RPC cycle.
- If update must continue while render has no credit, bound pending presentation state and retain reliable world changes. Do not create an unbounded snapshot queue.

### Gate 3

Approve only if the target latency SLO improves without unacceptable input/packet age, display cadence, memory or CPU regression, and replay/teardown tests remain correct.

Otherwise record **P3 deferred with evidence** and ship P2.

A third frame credit, separate sim worker, upload-planning worker and parallel encoding are **separate future experiments**, not bundled P3 deliverables.

---

## Delivery and evidence requirements

For every phase:

- One initial review plus at most two focused re-reviews; unresolved blockers stop progression.
- Module declarations/implementations, CMake registration and umbrella exports change together.
- Keep helpers private; do not export a speculative scheduling framework.
- Refresh affected codemaps and topology diagrams.
- Orchestrator records build/test/format/style/inspection results and captures; skipped checks remain explicitly skipped.

### Precise evidence requests

1. **Before implementation:** record the baseline commit and reconcile the interrupted P0 attempt. Current `git status --short` shows no tracked C++ modifications, but that does not establish the provenance of prior edits.
2. **Before P1:** provide the pinned Slang-RHI backend’s fence, submission, acquisition-failure and surface-release contracts for the operations used by `Renderer::renderAndPresent`.
3. **Before P2:** identify the exact platform wait/wake mechanism and demonstrate graceful-exit/credit-return wakeups without blocking native event servicing.
4. **Before P3:** provide numeric SLOs, representative tail distributions, matched GLFW source/build identity, and the 12 ms stack with running/waiting/ready attribution.

**Final quality bar:** the safe serial implementation and the threaded implementation differ in placement—not lifetime assumptions, state ownership, or error semantics.
