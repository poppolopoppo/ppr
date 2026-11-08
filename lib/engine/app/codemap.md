# lib/engine/app/

## Responsibility

`engine.app` aggregates application lifecycle, display/startup settings, owned frame transport, platform/window services, input/player, camera, pass-owned rendering, and ImGui. It depends on core/math/shader/rhi/image/mesh, not sim or physics. `App.cppm` is the authoritative export list; `CMakeLists.txt` registers interfaces/implementations and external dependencies.

## Ownership and execution

- `App.Application.cppm/.cpp`: `Application` owns the immutable `ApplicationDomain`, platform, service store, `TimerManager`, renderer, optional `RenderLane`, frame reservation/settlement state, and display controller. Rendering-enabled non-headless applications default to a dedicated worker (`m_render_lane_enabled`); headless or explicitly disabled-lane applications initialize and render inline. `setRenderLaneEnabled` is a pre-initialization choice.
- `App.Application.Editor.cppm/.cpp`: `ApplicationEditor` implements `IClientService` and owns window/viewport, input context and mappings, player, camera/controller, TrianglePass, GridPass, ImGui service, and client-side staged payloads. Object ownership stays here while pass GPU mutation and renderer operations execute on their owner lane.
- [frame](frame/codemap.md): copied camera/window metadata, immutable render requests, detached UI, bounded transports and reliable settlement results.
- [renderer](renderer/codemap.md): device-facing submission, surface generations, GPU fences/credits, pass caches, GridPass and TrianglePass. Renderer boundary types contain no mutable camera.
- [input](input/codemap.md), [player](player/codemap.md), [scene](scene/codemap.md), [window](window/codemap.md), [platform](platform/codemap.md), [service](service/codemap.md), [ui](ui/codemap.md): owning subsystem details.

`App.Scene.World.cppm` exists but is neither registered nor re-exported; it is not a usable built world API. Its presence does not require an unrelated task to register or delete it.

## Runtime flow

Before renderer teardown, `settleOwnedDeviceWorkBeforeTeardown_` collects
derived-owned callbacks and compute while the lane/dependencies remain live.
Its proof is separate from the renderer proof. The default is idle; unknown
completion/throw follows the existing fail-stop boundary. The boundary applies
to normal finish, direct shutdown, exception and partial-initialization cleanup.
`collectOwnedDeviceWait_` pumps existing frame/native acknowledgments and waits
at the existing wake quantum without admitting gameplay.

Opted-in clients may supply `nextSimulationDeadline` and `serviceSimulation(now)`.
The main loop services due simulation before render-credit admission and during
cap waits, outside predicate callbacks, then retains the same render deadline.
The normal final-sleep path also checks due work before the single TimerManager
tick. Default null deadlines perform no additional clock read or simulation work.
Control pumping remains observation/ack work, separate from simulation mutation.

1. `Application::run()` re-arms its epoch and initializes. Platform and shader/RHI services bootstrap before renderer setup. The renderer initializes on the worker when that lane is enabled; headless rendering initializes inline. Editor initialization creates client objects, resources, input routing, window and viewport. Startup service references are captured for worker callbacks.
2. The main loop advances its explicit clock and services completions/display control. Admission happens before virtual client update. A capacity-busy frame collects native events without advancing client work and carries elapsed time to the next admitted update. Pacing deadlines stay anchored to the last admitted frame, so denied credits do not re-arm a full frame interval. Terminal failures stop client/render work; an empty `requestExit()` is graceful success, while an error exit retains its cause.
3. Editor update runs platform/input work, syncs viewport, advances the camera, captures an owned `FramePacket`, prepares client-side content payloads, and updates UI. Game overrides add simulation and staged grid/debug data. Main does not mutate worker-owned pass caches.
4. Editor render consumes the packet once, builds detached UI draw data on main, and packages `RenderFrameRequest` with owned grid/triangle staging and reservation identity. The worker (or inline reference) applies staging, updates passes from the packet camera, prepares cell uploads, and encodes grid + triangle followed by color-loading UI before one present. Zero-size and obsolete frames are explicit non-rendering outcomes.
5. Results distinguish CPU staging consumption, submission acceptance, sweep failure, surface generation and UI acknowledgments. A CPU consumption acknowledgment is not GPU completion. Accepted submits stamp all participating resources even if presentation subsequently fails. Reservation accounting and GPU fences govern reuse; transport capacity is not completion proof.
6. `finishRun_` retains the first error through GPU preparation, shutdown and logger drain. Teardown stops admission and controls, settles queued/in-flight work, and keeps pumping native events during worker waits. It proves GPU idle or confirms removal before releasing resources. Unknown completion invokes the fail-stop boundary before ordinary destruction. Pass/surface/renderer cleanup runs on the owner lane; windows/platform cleanup runs on main. The worker joins before its storage/services die. A new run creates a new lane instance and epoch.

## Settings and transactions

- `App.Display.Settings.cppm/.cpp`: typed display schema and CLI bindings.
- `App.Display.cppm/.cpp`: requested/committed policy, provenance, pending candidate, acknowledgment-driven commit, effective pacing and degraded state.
- `App.Startup.Settings.cppm/.cpp`: startup-only core/GPU validation settings.
- [DisplayControls.md](DisplayControls.md): native mode/surface transactions, capabilities, Apply/Keep/Revert contracts. Game-specific drafts and persistent overlays live in `game/Game.Display.*`.

Display controls use owned lane-affine transactions; native window changes stay on main. Do not bypass generation/acknowledgment checks with direct cross-thread renderer writes. Default pacing follows vsync, and background throttling is centrally applied.

## Tests and registration

`lib/engine/tests/app/CMakeLists.txt` owns six serialized app shards, including display/window transactions, render lane and lifetime/failure gates. Asset tests cover GPU/asset integration. See [root tests](../../../codemap.md#test-infrastructure) for scheduling and loop policy.

`engine.tests.app.radiance_terrain_field_gpu` and its `_validation` partner
run the existing strict receiver oracle first, then the background physical
holdout, around one loop-local live fixture and producer. Their leaf names and
direct filters remain unchanged; a single-leaf run starts fresh. Both retained
receiver generations and the background view are checked after one final fenced
producer shutdown, before device teardown. Diagnostic writers are drained and
restored at each leaf boundary. Export/operator-qualified FP64 paths keep private
fixtures; failure/quarantine tests remain outside the shared scope. The provisional
3000s cold qualification bound covers observed native compilation cost, while
producer timestamps, warm submission/fence phases and independent numerical
verdicts remain visible. The former receiver/background ordinary entries are
replaced, so the primary matrix executes each scenario once per policy.

The opt-in `PPR_ENABLE_TERRAIN_CPU_PHYSICAL_COMPARISON` registers
`radiance_terrain_field_cpu_comparison` without a GPU/window lock. Its directly
filterable CPU-only leaf preserves the12 physical backwall points and both door
states. It compares direct1024 rays, actual terminal128 angular centres, unchanged
CPU C0, and one same-q C3..C0 dependency closure reconnecting unchanged C4 near8.
Actual connector/positive-interval/coarse-read counts and CPU time are reported;
no global dense candidate levels, GPU initialization, shader change or solver
adoption is implied. Routine GPU filters explicitly exclude this diagnostic even
when its registration option is OFF. The15% L1/1% leakage gates remain unchanged;
a passing candidate still needs resource/work admission and tiled GPU evidence.

For source additions, register the interface and matching implementation in `CMakeLists.txt` and update `App.cppm` only when exporting the partition. `App.TemplateInstantiations.cpp` owns explicit template instantiations. Do not infer registration from files merely present on disk.

Graceful close can be deferred through the application hook before latching exit. Nonblocking control servicing runs independently of client frame admission and in pacer slices, allowing derived owners to drain asynchronous work without advancing gameplay. GLFW asks for main-window exit before marking it closed or notifying listeners; a deferred exit preserves the live window. Render results explicitly distinguish successful presentation from accepted submission and applied staging.


Application admission observations are opt-in and reset with the run epoch.
They borrow the already injected clock, retain only fixed-size counters/maxima,
and distinguish pacing/event pumping from frame-credit probes. Disabled runs
perform no additional observation clock reads.
The last admitted interval also accumulates repeated skipped attempts, native
busy waits and completion servicing. Main snapshots and resets it before the
next virtual update; its outstanding-reservation peak is sampled at attempts.

Opt-in grid observations also transport fixed-size CPU frame timings with frame identity and explicit-clock endpoints. The injected application clock must support concurrent `now()` calls while render observations are enabled and outlive the render-lane join. Surface timings contain their preparation/draw callbacks; they are CPU wall durations, not GPU timestamps.

Blocking teardown/display barriers explicitly admit an owned RenderLane operation
then wait on its exact ticket using the existing pump/deadline. Invocation/result
acknowledgement remains separate from GPU completion; unknown completion retains
dependent resources and follows the existing fail-stop boundary. UI frontend
release stays on main, GPU release on its lane. Ordinary frames stay nonblocking.

The opt-in real-world direct-window lighting mode reuses Editor's existing
candidate producer, retained pictures and actual final-surface readback. A pure
receiver-window planner combines visible lookup halo with explicit gameplay
receiver demands. Editor schedules at most one32² page from each actual render
request, never from a command-poll loop; only the fully validated dense rectangle
is published. Current camera constants reuse a covered field; uncovered demand
holds the completed picture. The frame transport owns demand/provenance values,
while Renderer remains content-free. See renderer/frame maps for numerical,
resource, optical4x and failure contracts and still-open full-world gates.

For the bounded three-receiver direct-window capture diagnostic, Editor enables
the existing final-page coefficient mirror only with startup surface readback,
world-radiance measurement and three explicit receiver demands. After a fresh
clean captured present it requires the exact active batch/generation/cohort and
its completed final-page fence before calling `readDiagnosticCoefficients`.
The existing API validates every mirrored row's status and finiteness; only
three CPU DC/status records escape, joined by frame/capture/fence identities.
At 128x128 rows the extra mirror and temporary CPU vector are each 1.75 MiB
(plus the 32-byte statistics prefix for the mirror), diagnostic cost only.
Pending/unknown/mismatched work never becomes a successful probe result; local
CPU storage and the strong generation lease retire before normal owner teardown.
No new shader, pipeline, public API, clock, consumer sampler or readiness claim.

For an explicit direct-window capture, `executeRenderWork_` records the actual
`retro_scene_draw` target extent and pipeline signature only after all scene
draws succeed. A fresh clean captured present must match the owned request's
particle world/publication/terrain-revision/joint-tick tuple, and its receipt
logs that same view's device epoch and metadata count (including valid zero),
actual scene sample count/color format and output extent, joined by the returned
pixels' completed submission fence, frame, capture sequence and lighting cohort.
These are stack-local CPU facts; no public result layout, GPU lease or submission
fate changes. Retained pictures and failed/unsubmitted frames cannot supply this
proof. The separate Game receipt checks advancement beyond initial attachment.

### Phase-A direct-window discovery measurement

Only the existing explicit direct-window capture with world-radiance measurement
enables contributor discovery. TerrainLightingState owns the current direct
generation and immutable discovery result independently. A query-window change
first cancels/drains discovery, then admits replacement direct pages as needed,
then discovers the exact requested geometry. Shared submission-fate settlement
stamps discovery separately from the direct field fence; unknown retains owners.
The existing per-frame pending/error result is stronger on this route only:
readiness requires matching source/cohort/window and complete discovery. Held
pictures never claim it. Frame tokens bound one 262144-trace quantum, never on a
field-page frame or from control polling. First clear/counters/copies/timestamps
are included. Setup CPU time, cold age, warm age, actual rays/segments/face counts,
unique offscreen counts and admitted owned/overlap bytes are reported; reserve
is explicitly estimated, measured VRAM and whole-pipeline qualification stay open.
A GPU/storage/completeness failure is NO_GO. Warm age failure remains visible but
does not discard a functionally complete B capture. Existing fences, release and
positive-quiescence teardown apply; no moments or field composition is added.

Direct-window fresh strips and contributor discovery use the SAME existing
actual-frame work stamp: set it before either submission, pass the packet frame
ID to direct encoding, and reject a second quantum on that request. Direct work
still completes before discovery preparation; polling only settles accepted
fences and never admits work. Per-frame acceptance joins all actual direct/copy/
discovery intervals by that stamp, not independent 4ms budgets. Missing timing,
CPU uploads, complete lighting cost, cadence and full VRAM remain open gates.

The opt-in `single-bounce-window` uses the existing direct-window command and
render-owner schedule. Editor keeps pristine direct and complete combined leases
separate, schedules discovery then bounce through the SAME actual-frame token,
and holds the previous picture until the exact combined source/cohort/window/
epoch completes. Direct three-point diagnostics still read the pristine donor
and label the actual consumed field fence separately. A/B uses the existing
Game camera controller/capture receipts; no Renderer camera ownership or new
public frame-camera payload is added. Component6ms NO_GO does not qualify total
lighting/cadence/age/VRAM; all broader pipeline gates remain open.


## Committed-Matter optical epoch scheduling

The existing Editor1x1 lighting submission/fate path also runs the pass-owned
immutable Matter projection before direct/discovery/single-bounce production.
All phases share the same actual-frame token. An accepted snapshot/field finishes
while later current views coalesce. Its direct-admitted query rectangle remains
fixed through discovery/bounce; an uncovered latest window proceeds after that
work completes even without an intervening present. A completed compatible field
may draw fresh frames during replacement only after current demand coverage is
proved, carrying its own unchanged receipt and a pending flag. Stale draw packets
do not block accepted lighting settlement and never draw obsolete particles.
Initial terrain activation remains provisional until an actual committed view (including count0)
is projected. The canonical static root is bound once to its real Matter terrain
revision; a changed revision requires new coherent terrain staging.

Oldest delivered source-request time and the first change after a frozen epoch
are separate. Only a fresh presented field with the exact bound source/fence
advances incorporation. These are request-observation ages; full edit/commit age
is unmeasured. Completed projection survives field admission retry. Unknown fate
retains existing quarantine/teardown semantics. Editor-flow empty/retry/watermark
branches are source-reviewed until actual Application.run evidence; manual
GateEditorApp ticks do not advance the real frame counter.

For the explicit single-bounce temporal capture, a fresh covered pending field
is eligible only after a prior accepted TSR value matches its actual bound
publication and raster/output extents. A replacement's first frame cannot consume
the one-shot. Final accepted history/pixel fence proofs remain mandatory. The
optional latest-donor three-point readback is omitted and labeled unmeasured on
this covered-pending capture; it cannot describe a different bound old field.

Fresh Editor scene encoding first prepares admitted triangle material variants
using the shared scene draw callback's actual `DrawContext` pipeline signature.
This covers native and Retro offscreen scenes before fresh scene acknowledgement,
including empty triangle stages. Retained-picture rendering bypasses this work.
Preparation errors propagate through the existing scene failure path; CPU
preparation cost is included in triangle observations.


Editor prepares the required WorldRadiance discovery pipeline after successful
field admission and before candidate field state/encoding, only for the existing
single-bounce or direct diagnostic discovery route. Ordinary direct-only setup
retains lazy discovery compilation.

Temporal RC prepares carry the injected application/frame elapsed time into probe
sample metadata. Matter updates certify the same visible/gameplay window they
recompute. A camera demand outside the completed certificate may start a recovery
cycle before first-present acknowledgement, preserving the original outstanding
request time; otherwise acknowledgement remains the gate before another cycle.
Bootstrap requests visible/gameplay demand with complete parent closure. Recurring
cycles prioritize missing demand coverage, then alternate demand convergence with
32-cell background tiles. An empty demand selects a bounded tile. Under continuous
Matter updates, the editor can include one offscreen tile with the mandatory
visible/gameplay window in the same frozen source. It compares recent measured
service durations with the time left for both that request and an immediately
queued successor. Only a successful field completion advances the tile cursor.
Refresh completion and round-age observations expose service and deferred work.
Optional convergence uses GPU age and normalized coefficient change to select
refined probes. A bitwise-equivalent GPU Matter projection can retain its
predecessor's transport version when the compared identity matches the published
donor. New optical sources, required coverage and rotating refresh tiles retain
their full dependency work.

When a sky change arrives during a frozen Matter update and a successor is
already queued, the completed spatially covered field may present. The next
Matter field carries the pending sky hour. Moving outside the certified window
still requests spatial coverage immediately. Gameplay acknowledges a sky update
only on the matching presented field. Temporal viewport correction starts with
the first active lighting field, including the frames before driver attachment.

Before each temporal field or Matter submission, Editor reserves the maximum of
eight recently measured whole-surface costs against the 60 Hz frame interval.
Separately, it reserves screen lighting against the map's 6 ms or 10 ms
allowance using measured screen costs, calibrated terrain material credit and
any due calibration work. While the first frozen Matter projection awaits
presentation, the screen peak covers four completed samples (one query-slot
rotation) so a startup clock transition ages out promptly. Subsequent updates
use eight. Both the whole-surface guard and terrain credit history span eight
samples throughout. Matter and field work share the actual-frame token. With
zero allowance no new producer work is submitted. The first producer timing
is unmeasured until its GPU query resolves.

Render-lane activation can precede the driver’s joint terrain/Matter commit.
The editor checks the packet’s expected lighting revision before admitting
its Matter snapshot or updating the view. A packet from before the edit
therefore keeps its old particle view out of the newly activated optical source.

Temporal screen timing uses four fence-owned query ranges. In addition to the
usual preparation and mixed draws, a sampled frame measures one offscreen
terrain-material stripe including its fixed draw overhead. The editor visits
all sixteen bands in pairs of consecutive draws, then refreshes one band every eight frame IDs. The
cost estimate credits 80% of the lower of the two latest net times in each
band, capped at 80% of the lowest recent full terrain cost. New optical
cohorts, output sizes, zoom/orientation changes or camera translation beyond
the sampling footprint invalidate the estimate. Older pending samples cannot
restore credit after invalidation. Background, particles and screen correction
are charged in full. Calibration overhead is reserved on sampling frames, and
the whole-surface constraint is applied independently. Incomplete accepted
timestamp ranges remain unavailable until their fence retires. Idle-proven shutdown
collects outstanding screen timings before releasing the timestamp pool.

A newer edit of the same world can replace a temporal window at its last
accepted GPU fence. The next revision keeps the displayed generation as its
lighting history. The interrupted output has no presentation or gameplay lease.


Temporal gameplay demand stays active until settlement. A distant request has
its own cascade dependencies and GPU coverage check alongside the visible window. A matching presented generation delivers its bounded CPU coefficient
snapshot through the frame result. Supplemental background work yields when the
oldest frozen request or its queued successor reaches the service margin. The
render lane retries the same tile after yielding, and advances the cursor only
when the whole tile completes.
