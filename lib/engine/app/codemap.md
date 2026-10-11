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
