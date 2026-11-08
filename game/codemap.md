# game/

The explicit direct-window A/B capture protocol records the first matching B
present age before readback or history eligibility. When temporal2x is actually
enabled, a later matching B observation must prove real bound history with an
earlier accepted fence, distinct slots and the same immutable lighting publication
before capture is armed. The captured frame independently repeats that exact-fence
join with its post-Matter pixels; per-pixel history influence remains unmeasured.
This observational wiring does not enable the mode or change the spatial control.

## Responsibility

`app.game` is the reference client for PPR: an interactive generated colony plus headless smoke checks. It owns game state and policy, not engine infrastructure. `demo::TurboLarbin : ApplicationEditor` owns the colony driver, simulation packet, tool/selection/follow state, game input mapping, and display drafts/persistence. The editor base owns window, player, camera, viewport, UI service and both render passes.

## Entry and configuration

- `main.cpp`: thin entry point calling `runGame` exported by `game.application`.
- `Game.Application.cppm/.cpp` (`game.application`): resolves invocation through `Game.Config.*`, loads the display overlay through `Game.Display.*`, applies explicitly supplied startup validation settings before GPU work, and dispatches the selected command. Help is generated from schema declarations; invalid usage exits 2. Smoke returns its error value. Interactive `app.run()` errors are printed and returned; unexpected exceptions escaping that boundary invoke `hal::fatalExit`.
- `Game.Config.cppm/.cpp` (`game.config`): typed game command/profile/exit schema, combined CLI and configuration resolution. Distinct command selectors conflict; they are not gameplay toggles. Use `--help` and declarations for the current option set.
- `Game.Display.cppm/.cpp` (`game.display`): draft validation/provenance, persistable overlay merge/encode/atomic write, overlay load, and injected-time Keep/Revert state. Overlay precedence is base < overlay < environment < CLI. The panel produces intents; application-level generation/ack checks govern native/surface changes. See [DisplayControls](../lib/engine/app/DisplayControls.md).
- `Game.WorldgenWorkshop.cppm/.cpp` (`game.application:worldgen_workshop`): authoring session, immutable worker requests, bounded cancellation, preview/export and owned `AuthoredGameSource` preparation. Standalone Start validates the current owned snapshot through the existing WorldJob; publication commits its source for later New Game. Return restores the retained game while preserving the authoring session. See [Workspace usage](../docs/worldgen-content.md#in-game-worldgen-workshop).

## Interactive flow

1. Initialize the editor and lane-affine game matter runtime, then request a candidate session. Independent main-owner simulation service admits worker generation and publishes the CPU driver after acknowledged simulation-resource preparation; a loading presentation is telemetry, not admission permission. Kenney assets are staged but no committed fixture scene is loaded.
2. The main-owner simulation deadline runs independently of render credits/drawable targets, using explicit elapsed admission and pending fixed slices. Presentation update advances platform/input/camera/UI and captures only committed owned `SimFramePacket` state. Preparation validates tick/world-generation coherence and builds owned `GridStagedFrame`; debug instances become `TriangleStagedFrame`. The UI uses client data and result-fed render diagnostics, not live worker caches.
3. Editor render sends owned frame/pass/UI payloads to the render lane. Grid/triangle pass mutation, upload preparation, encoding and presentation run there. Inline hosts consume the same request structure without transport.
4. `game.application:session_service` owns typed control/result delivery. The base frame-credit receiver settles immediately; game acknowledgments, UI controls and native close wait for exclusive main service through GPU publication and dependent Scene commit. FIFO controls preserve repeated toggles/steps; native close is an unordered safe-point latch. World/job stamps reject stale orders. Dropped or unapplied presentation payloads remain pending for a real later frame.
5. Shutdown stops mutating-control admission, detaches/drains matter on the live lane, proves independent frame/control completion while retaining CPU header owners, closes the finite result receiver, drains detached worker owners, and finally delegates service/window teardown. Unknown completion fail-stops before dependent release.

The resident solver owns current/candidate numerical particles. Normal ticks
return compact status, check the captured body epoch, commit one-way Scene
coupling and step Box2D once; normal frames carry a shared GPU view without a
CPU particle mirror or batch upload. Metadata prepares only at world/inventory
admission. Checkpoint, preview, inspection and edit coverage request exact demand
snapshots on existing workers, with simulation admission frozen while UI/render
continues. Complete v6 payloads and atomic file saving remain unchanged.

Preparation readiness separately matches an actual-present resident stamp for
the acknowledged world/job/generation, publication, terrain revision and joint
tick. CPU preparation, queue acceptance and GPU completion do not release it.
The initial expected publication is held stable while the modal is open.

Controls and panel intents are declared in `Game.Application.cpp` and `ui/ColonyPanel.*`: pause/step/speed/regenerate, inspect/dig/build/demolish, agent selection/follow, grid/edit overlays, help, independently visible Colony/Performance, Display and Menu Quit. Regeneration is confirmed and deferred to an update boundary; Quit and native close share the host's dirty-draft/cancellation/drain path.

When the session has a controlled actor, WASD supplies held fixed-tick commands
ahead of camera input. Opposing directions cancel. Focus loss, UI keyboard capture,
pause and new-game requests clear the command. Camera input rates reset when
gameplay retakes key ownership while preserving Inspector follow; arrows and
mouse remain camera controls. Focus centers the metre-sized actor at inspection
zoom and refreshes the viewport offset on resize. Cancellation restores the old
session with a neutral status. Asynchronous close keeps the active render bundle
while the worker disposes the CPU session, rather than applying an empty catalog.

## UI ownership

See [ui/codemap.md](ui/codemap.md). Game-owned presentation now lives under
`ui/`, with stable existing panel module names. `game.ui.model` captures only
queried cells, scalar reports and cached panel-safe agent summaries before the
synchronous draw. Names/rows are valid for that call only; actions dispatch after
it. `game.ui` owns modal/feedback drawing, while this host owns world/camera
projection, simulation, lifecycle and workshop/display transactions. World-job
progress vocabulary lives in `colony/WorldProgress.cppm` independently of the host.

The merged workspace keeps draft index/qualified asset identity in game UI and
defers selection until old controls emit owning edits. Host mutation dispatch
precedes Start and rechecks current validation. A preview-drain phase settles
shared render commands before a generation phase exclusively pumps WorldJob and
loading presentation. Its per-request callable owns the authoring snapshot;
cancel/failure preserves editing, publication commits the future game source.
Return retains driver/camera/pause, Quit uses the unsaved transition. Gameplay
chrome reserves the left rail in picking; native input capture protects the
summary overlay and background feedback cannot cover opaque windows.

## Colony modules and workloads

Matter lifecycle requests continue across main-service turns on the existing
single RenderLane operation slot. Session mutations retain their FIFO/cursor
while frame/grid acknowledgments and the independent close latch remain
collectable. Slot readiness drives notified continuation eligibility; only GPU
fences and existing worker/tool predicates use timed polling. Workshop return
retains its own resume/finalization continuation. Game settles Matter before
the base renderer teardown proof and lane stop.

See [colony/codemap.md](colony/codemap.md) for the `game.colony.*` modules. Sim numerical state remains renderer-independent; the game driver and `SimFramePacket` bridge the explicit renderer-neutral GPU view to app rendering. Physics Scene and ChunkColliders are driver-owned; Box2D stays inside `engine.physics`.

- `--smoke`: separate headless rendering-enabled Application, generated-cell offscreen readback.
- `--auto-exit` / `--exit-after S` bound graceful runs; the retired `--profile100` option is rejected.
- CSV timing, scripted profiling edits/pan, load/time-trial commands and game-local exception interception are retired. HAL profiler scopes and crash handling remain.

## Build and tests

`CMakeLists.txt` registers the colony modules including `game.colony.matter`, plus `game.config`, `game.display`, `game.application` and its `:world_job`, `:worldgen_workshop`, `:session_service` partitions, and thin `main.cpp`. `app.game` links core/app/math/shader/rhi/sim publicly and physics/matter privately. `Game.SessionService.Tests.cpp` exercises the actual production inbox through a real render-lane wait pump and stale tool delivery; it is registered in `game.tests`. Every explicit executable build stages shaders, textures and meshes when present through the independent `app.game.assets` prerequisite. Asset edits, additions and missing deployment files do not require relinking; files removed from source are pruned from deployment.

The game build registers `game.tests`, the split `game.tests.worldgen_preview`
entry, three independently locked GPU lifecycle entries
(`game.tests.matter_edit_gpu`, `matter_coverage_gpu`, `matter_restore_gpu`), and
the `app.game.worldgen_frontier`/`worldgen_hollows` profile validations unless tests
are explicitly disabled. The CPU entry excludes `game/checkpoint_gpu` and the two
heavy preview replay/crop leaves; the worldgen entry uses `PROCESSORS 4`.
Each GPU entry has its own 180-second ceiling and unchanged explicit validated
D3D12/4103-grid/eight-substep/K2 fixture. `game.tests` stages its shader assets
independently. `Game.Config.Tests.cpp` covers invocation compatibility, source
precedence and help; `Game.Display.Tests.cpp` covers drafts, persistence and
Keep/Revert. Engine test targets never depend on these game modules.

The game-owned world coordinator retains immutable requests, job-local cancellation and isolated candidate drivers. Independent worker admission is explicit; default WorldJob callers retain the original loading-presented gate. Cancel/failure before renderer activation admission preserves the old session; activation admission latches commit intent, and post-activation attachment/presentation failure is terminal while owners remain retained through real retirement. Only detached drivers may enter worker cleanup/capture. Normal startup loads the authored pack/profile/seed and initializes one actor. Independent render controls stage/activate owned terrain separately, and the modal remains through real presentation. No frame result or loading acknowledgment is fabricated. Integrated responsiveness remains unverified: cold command finalization occupies the sole device lane for about40s. Matter now uses retained asynchronous operations; the renderer retains its existing synchronous teardown policy. No timeout, resolution, schedule or backend workaround is adopted.


`--worldgen-preview --worldgen-output=<directory>` exports material/biome/temperature/initial-matter P6 PPM overviews and a bounded JSON report through `colony/WorldGen.Export.cpp`. It is a headless authoring path; see `docs/worldgen-content.md` for resolution, orientation and baseline semantics. Mandatory current-state checkpoint envelope v6 is owned by `game.colony.checkpoint`, with dimensioned raw sim snapshot v5. Historical seeds never replace evolved columns; detached restored owners stay frozen until runtime attachment.

`--worldgen-preview --worldgen-gpu-output=<directory>` explicitly enables a rendering-capable,
UI-free frozen diagnostic host after generation. `colony/WorldGen.Capture.cpp`
uses the real GridPass, offscreen render targets and GPU readback for metre-based
start/seam/surface/texture closeups. Blocking upload/completion is restricted to
this host; GPU quiescence is enforced before releasing resources. Explicit RHI
validation settings apply before initialization. See [authoring diagnostics](../docs/worldgen-content.md).

`--lighting-scene --worldgen-gpu-output=<directory>` selects a separate handcrafted
bounded lighting/reference scene, never generated world output. It uses the
existing selected pack/profile and typed startup validation before a UI-free
capture host in `colony/LightingScene.Capture.cpp`. Catalog validation and prepared
appearance are reused without generating a complete simulation world. The
64x36 sample geometry, its matching Grid upload and the receiver producer share
one immutable geometry owner per closed/open door state. Real RC completion and
status validation precede Grid receiver binding; the actual opaque GridPass scene
then uses 1920x1080 MSAA4, current-frame Hable 2x reconstruction and EV/AgX to write
3840x2160 `door-closed.ppm`, `door-open.ppm` and `lighting-scene.txt`.

The CLI-only `--lighting-scene-qualified-fp64` boolean is default false and
rejects the option's presence outside this capture command, including explicit
false. Selecting true is an explicit operator
assertion that RTE64 is qualified for the current Vulkan device/session, passed
through the existing capture boundary into the reviewed experimental FP64
refinement policy. It performs no automatic device/driver selection and is not a
persisted game setting. Vulkan/Double and precision admission failures still
propagate normally. Without the flag, existing three-argument capture callers
remain strict FP32. The manifest records selected precision/operator assertion
and each completed immutable generation's attempted/qualified/unresolved/bound
counts, or honest absence in strict mode; publication validation is unchanged.

Five captured sediment/ore rows preserve their authored appearance. A distinct
`scene_amber_lamp` row copies magma texture/tint/emission with an explicitly
scene-only opaque policy; shipped magma is unchanged. The lamp is vacuum-isolated,
the wall is one cell thick and its 1x8-cell aperture changes eight door labels.
This scene does not qualify a 1x1 portal or the remaining phase holdouts. The manifest records
geometry/presentation cohorts, receiver status-derived right-floor response and
palette provenance. This blocking diagnostic is default-off, fails on unsupported
capabilities or invalid RC output, and does not admit ordinary-world lighting,
matter/media, directional normal/specular lighting, bloom, temporal reconstruction
or frame/memory budgets. The headless scene has no UI; it does not independently
qualify native UI. CLI tests cover output requirements, selector conflicts and
the explicit handcrafted-scene help contract. Source/execution and image review
remain required before claiming the capture succeeded.

The opt-in `--worldgen-acceptance` command is an interactive production-path
acceptance sequence owned by TurboLarbin: stationary/pan/zoom, a presented seam
edit, preview switches and two acknowledged New Games with resource retirement.
It requires authored sessions and reports incomplete shutdown as failure. See
[worldgen authoring and diagnostics](../docs/worldgen-content.md) for scope and
measurement limits. `--worldgen-report` mirrors render-owner observations through
frame results; neither option reads worker-owned pass state from main.

The world-work modal displays one stage-local progress bar: measured raster/staged chunk counts when available, otherwise an explicitly indeterminate animation driven by the injected application clock. It does not infer overall completion or number preview/cleanup states as generation steps.

## Joint world transition

`Game.SessionService` owns value-only transition phases used by the real host
and focused tests. Replacement snapshots/logically detaches the old driver
before candidate GPU preparation, keeps the old Runtime owner valid through
reversible preparation, and treats renderer activation admission as commit
intent. Its exact activation ACK precedes candidate attachment and the
exactly-once WorldJob/host ownership transfer. Old GPU view selection is disabled
between activation admission and joint publication; ordinary frames remain
nonblocking. Pre-activation cancellation drains both candidates before resuming
the retained identity/pause/readiness facts. Post-activation attachment or
unknown completion errors retain the established terminal/quiescence policy.

Workshop Return resumes/binds the retained driver and restores the camera while
remaining frozen. It emits the restored resident bundle with progress UI and
finishes only when the delivered actual-present stamp matches its reservation,
world/job/generation/fingerprint, publication, terrain and joint tick. Activation
alone cannot clear the modal. Close may abandon visibility after owned operations
settle, then follows normal detach/drain. Normal simulation does not wait for
presentation after this one-time transition.

Workshop entry holds an O(1) strong copy of the final jointly committed particle
view before transferring Driver to the capture job. Retained game frames select
that exact lease through `selectWorkshopRetainedParticles`; otherwise a final
publication can revoke the pass's older cached view and reject the whole frame,
including detached UI. Selection requires the retained game identity and stops
at preview activation admission, preview display, finish or another world
transition. The held copy clears at activation/new-world admission, completion
and terminal cleanup; existing renderer epoch/world checks and fence-held readers
remain authoritative. The current correction is implemented pending focused
execution and unchanged interactive verification.

Rollback's retained resume phase owns Driver service through its exact Runtime
acknowledgement. `sessionTransitionOwnsDriverService` routes the host before
ordinary Driver update, including a latched close; the logically detached durable
owner is never updated through the generic path. The route relinquishes service
immediately when old identity/pause/readiness restoration acknowledges. The
existing checkpoint GPU restore leaf uses an admitted queued/ready resume across
multiple host-style turns to exercise that same production routing predicate.

The startup map size (`--map-size` / `game.map_size`) is 1024, 2048 or
4096 fine cells per edge, default 2048. Explicit per-world bounds travel through
generation, prepared descriptors, grid/particle state and checkpoint formats;
maximum-stride chunk identities remain stable. Smaller maps retain the same
physical cell and actor scale.

The Workshop Map size combo owns a session choice initialized from startup.
Changes invalidate compiled validation and previews while preserving authored
bytes, undo history and source epoch. Preview and Start New Game compile the
selected extent; later New Game uses that owned authored snapshot's extent.
Returning to a retained game or restoring a checkpoint keeps its saved bounds.

Simulation tools share the existing world-owned pointer gesture. Push and pull
apply a held radial acceleration; spawn and destroy admit one owned particle
inventory stroke per press. The tool context configures radius, acceleration,
spawn count and catalog particle material. Startup defaults are available through
`--tool-radius`, `--tool-strength`, `--spawn-count`, `--spawn-material` and
`game.tool_radius_m`, `game.tool_acceleration_m_s2`, `game.tool_spawn_count`,
`game.tool_spawn_material` in TOML/environment. UI edits are session values.
Force is cleared on release, cancellation, focus loss and tool/world changes.

## Experimental playable lighting showcase

`--lighting-showcase` selects the normal interactive TurboLarbin application with
a game-owned, catalog-backed 64x36 authored island inside a normal simulation grid.
The existing WorldJob, Driver actor/colliders, native UI and camera controls remain
the owners. It uses the bounded native-cell `straight256_reference` world field,
signed directional harmonics and a receiving backwall. The old exact
contour/precision paths remain separate diagnostics. The direct control passed
5.7851% aperture L1 and zero blocked-side leakage; scalable cascades and full-world
performance remain unqualified.

Activation waits for a matching completed immutable field/Grid cohort. Pan/zoom
reuse that field and update camera draw constants; no exact per-view retracing is
performed. World preparation/backpressure can retain the previous complete
picture under live native UI. Picking uses its displayed camera/world/extent.
Native pipeline creation occurs during loading. Editing, particle tools and
checkpoint/workshop transitions remain unavailable until coherent optical edit
transactions are added. Actor walking and ordinary pan/zoom remain enabled.

After the matching candidate activation acknowledgment, the static showcase builds
only current actor/debug/UI metadata per frame. It does not recopy the selected
catalog, resident cells or converted uploads. Any newly dirty terrain remains
unacknowledged and fails the static checkpoint admission. Displayed update age
uses the displayed view/picture request timestamp while the latest camera differs;
a stationary matching view does not age merely because time passes.
That view-mismatch age is distinct from the stable world-source stamps exposed
by the opt-in measurement path below.

During a pending resized view, the old picture remains at top-left native pixels
with clipping/letterboxing. Picking uses the old camera and picture viewport,
converted through current framebuffer/client scale, and rejects the clipped or
letterboxed region. Native UI continues to use current window coordinates.

## Interactive showcase frame capture

`--lighting-showcase-frame-capture=<path.ppm>` is CLI/startup-only and requires `--lighting-showcase`. It enables final-surface readback before Renderer construction, arms once after a current showcase presentation, and writes the next eligible actual frame including native UI. Game owns P6 file policy and logs actual extent/format/row pitch/fence/frame/cohort/camera identity. RGBA/BGRA display-encoded bytes receive no additional exposure or gamma. The caller supplies an existing writable parent directory. Failed attempts report an error and are not retried per frame; no separate viewer/headless scene is used.

Capture eligibility is attached to the actual playable UI frame: Game resets it at every update entry and enables/arms it only after a matching first-presentation ACK, progress dismissal and successful normal panel construction. A field generation alone cannot qualify a loading/closing/workshop image. The existing session owner exposes the used readiness predicate, covered through the real presentation matcher. Showcase Pan bounds now use the admitted source crop's own origin, dimensions and metric; the existing larger-than-bounds rule centres it at the fit scale. Normal worlds retain full-grid bounds, and no camera/controller matrix semantics changed. The first modal readback is retained as an ineligible artifact, not an A/B result.

Progress dismissal is also checked against the actual UI draw result. The
closing frame can have a logically closed owner and still begin/close the
ImGui progress popup; that frame remains ineligible. The successful playable
branch passes this local draw fact to the used Session readiness policy, with
no frame delay, persistent popup state, or render-packet ABI change.


The explicit reference scene now authors one industrial service bay through
`lightingShowcaseLabels`/`prepareLightingShowcase`: a level tread connected to the
spawn shelf, supported lamp housing, oriented pipe/elbow and an open-frame cyan
opaque-proxy tank beside a tapered sediment/ore bank. All obstructions use the
same catalog-backed native cells as Grid, optical capture and Driver colliders.
Camera, EV3, cut-face0.04 and source strengths retain the ready pre-bay baseline.
The amber fixture now mounts directly beneath the central ledge, leaving both
sides and the region below open for the receiving wall and walkway.
This static art checkpoint adds no door/source mutation or production-scale RC
claim. The real Driver walking/tank-contact regression passed for both lamp
placements; a ready 4K under-ledge capture now shows the warm receiving wall and
tread. Reference-art acceptance and dynamic door/source transactions remain open.

## Showcase measurements

`--lighting-showcase-metrics` is default-off, requires the showcase and excludes
`--lighting-showcase-frame-capture`. It leaves `--worldgen-report` unchanged.
The uncaptured mode enables the producer's batch-local GPU timestamp pair and
joins delayed Renderer timings to exact recorded ready-frame submissions. It
uses a two-second ready warmup and 256 unique matched samples, with bounded
provenance storage and explicit excluded/missing observations. Window world,
extent, requested VSync and actual surface identity remain fixed; interruption
preserves the original identity and marks the window incomplete.
An ineligible packet is excluded without interrupting an identity-stable window.
The reporter distinguishes all clean-ready joins from warm clean-ready joins,
records provenance misses, and logs the first skipped packet and typed identity
interruption reason without per-frame logging.

Reported Renderer timing includes scene, resolve, reconstruction, tone mapping,
UI and other commands in that submission. It is not isolated lighting/scene-draw
cost, independent matter work, CPU preparation or physical scanout. Producer
timing has its own narrower scope; source-command, publication and first-present
elapsed stamps use the sampled Application clock. Static source age does not
qualify dynamic edit latency, and resource ledgers do not measure VRAM.

## Mechanical service door

The lighting showcase's native top bar exposes Open/Close service door and
pre-activation Cancel. TurboLarbin reuses the single owned Grid-world command
slot and its real acknowledgment delivery. It holds actor simulation, preserves
the completed picture beneath live UI, prepares the exact candidate/source,
activates matching Grid/lighting, then authorizes Driver commit only from that
activation's outer revision. Simulation resumes only after a clean actual-present
result consumes that revision; a retained picture reports its older revision.
Preparation failure/cancel preserves the committed cohort. Sent activation with
unknown outcome is resolved before cancellation, another attempt or teardown.
Other showcase mutation/save tools remain unavailable.

`--lighting-showcase-door-check=<existing-directory>` is a default-off interactive
acceptance sequence, mutually exclusive with one-shot frame capture and metrics.
It uses those same intents/Driver PlayerCommands, captures `service-door-closed.ppm`
and `service-door-open.ppm` before crossing from the left side, tests closure on
the walking actor, crosses the open door and presents the final closed revision.
Both images retain the same actual camera/EV3/sources. The 180-second operational
ceiling starts at the first Game update, including loading; success requests normal
exit and interruption/failure remains an error. No generic exit timer is needed.
The reported request-to-present latency starts at admitted intent and ends at
main-side receipt of the first clean matching present, before diagnostic readback;
it does not measure physical scanout. The original physical lighting gates remain
separate from this after-tonemap visible comparison.

Door teardown settles matching graphics acknowledgments from both the inbox and
already retained delivery tail before interpreting a transport/receiver failure.
If ownership is still unresolved after a pump or latched inbox/stash error, it
returns the existing unknown-quiescence result with the first error and retains
the binding; it cannot wait forever, infer cancellation or reuse the identity.

### Backwall screen-contact comparison

`--lighting-showcase-screen-contact` is startup-only/default-off and requires
`--lighting-showcase` even when explicitly false. It selects only Grid's bounded
backwall pilot through the existing owned staging request; it does not change
scene cells, source strengths, camera, EV3, cut-face0.04, foreground shading or
world transport. The fixed native ROI[34,38)x[7,15) projects to the current
low-resolution raster and is capped to128x256 evaluated centres; outside is
explicit world-only control. It combines with the existing actual one-shot or
door-pair capture APIs, not a new viewer/capture path. The ordinary physical
15%/1% masks remain separate and unchanged.

Pilot and door-check enable existing per-generation WorldRadiance GPU timestamps
without allocating the separate256-sample metrics/auto-exit coordinator. Timing
logs distinguish producer GPU scope from CPU prepare/publication/present receipt.
Screen visibility is sampled approximate debug presentation; fine-image quality,
performance and scalable world transport remain unqualified.

### Published showcase material observations

With `--worldgen-report` and `--lighting-showcase`, the once-per-world actual
publication branch reports sandstone albedo/normal images from the owned
`PreparedWorld::presentation()`. Material lookup uses the selected catalog key;
texture lookup uses its resolved qualified identity and normal interpretation,
the same mapping as `SimFramePacket`. Records include the actual prepared image
format, dimensions, mip count/layouts, source fingerprint, period, sampling mode
and normal scale. Layout reporting is capped at 16 rows per texture and raw RGBA8
reporting at 4 checked texels per texture. Other formats or unavailable storage
are reported explicitly, never reinterpreted. These are CPU preparation records,
not proof of sampled GPU orientation, texture LOD or visual material quality.

Existing service-door milestone/latency records remain authoritative for the
Game intent, revision, activation, commit and matching-present boundaries; the
new sparse Editor/producer records fill their internal preparation gap without
a second Game trace or an implicit clock.

## Mounted amber lamp switch

The explicit lighting showcase adds a top-bar amber lamp switch beside the service
door. Both use the same single Driver-owned service-bay publication slot and
never-reused optical revision; the existing door wrappers and cancellation/unknown
submission contracts remain in force. The lamp changes only a copied amber visual
row's emission strength (authored ON, zero OFF), with unchanged native cells,
colliders, images, camera, exposure and other sources. Source capture derives
optics from that exact projected row; no catalog, pack or renderer policy changes.

`--lighting-showcase-lamp-check=<existing-directory>` is mutually exclusive with
door-check, one-shot capture and metrics. The existing private game witness uses
the same panel intent/Driver transaction, programmed neutral command receipts,
fixed camera/actor anchor and protected readback rearm for
`service-lamp-on.ppm`, `service-lamp-off.ppm`, and
`service-lamp-on-restored.ppm`. It waits for each actual matching clean present
before readback, records OFF/ON intent-to-present receipt ages independently and
keeps captures when the one-second target is missed. Its existing 180-second
ceiling starts at the first game update; functional completion requests normal
shutdown. Receipt timing is not physical scanout/native input latency, and this
small-scene reference does not qualify scalable transport or the full budgets.

## Opt-in generated-world direct-window lighting

`game.world_lighting` / `--world-lighting=legacy|direct-window` is typed startup
configuration; legacy remains default. Direct-window selects the actual generated
frontier/hollows session, not the showcase. The first component measurement uses
explicit BLACK boundary sky, static terrain optical input, homogeneous absorption/
emission and no scattering or dynamic particle optical sources. Actor/camera and
ordinary matter simulation/presentation continue. Terrain/source mutation tools
and Worldgen Workshop are explicitly unavailable in this measurement mode;
ordinary-world edit integration remains required before shipping/default use.

WorldJob owns complete source capture before transferring the candidate. The
initial direct-window camera reuses actor-inspection zoom and lower-left eye
framing at the actual generated spawn; it never moves the actor. The actual
page rectangle and separate ALL-PAGES resource admission outcome are logged.
All-pages admission failure does not qualify the4GiB/4ms/1s/full4096-world gates.

`--direct-window-frame-capture=<path.ppm>` requires this ordinary interactive
mode and excludes showcase captures/checks/metrics. It reuses the existing
owned final-surface readback, playable UI readiness, matching revision/present
receipt and caller-owned PPM writer. Success records actual camera/cohort/fence/
extent/format/row pitch plus completed receiver coverage and requests normal
shutdown. It is not a native-input/physical-scanout latency or HDR aggregate
qualification. No viewer, screenshot renderer or new capture framework is added.

The committed direct-window session uses the existing retained-terrain/actor route
after joint publication, with the exact source revision stamped independently
from the Driver's mutable current state. Reversible replacement/cancellation does
not overwrite that committed stamp. A retained frame requires the same activated
world identity, unchanged source revision, no terrain dirt/reset/full refresh/edit,
and an attached current GPU particle publication (including a valid zero count).
The Editor still applies the actual per-frame particle lease and actor stage; a
missing/stale/foreign view cannot silently retain old particles. New terrain input
requires coherent restaging and is rejected in this static measurement mode.
The Grid ownership/policy guard remains unchanged. The actual generated-world
frame is the routing regression; dirty/stale/missing fault paths are source-reviewed
unless separately exercised, and no full-world budget is qualified by this fix.

The explicit direct-window frame capture with `--worldgen-report` also declares
three fixed native receiver centres relative to the actual generated landing:
(44.5,29.5), (42.5,6.5), and (66.5,6.5). Game derives the global origin from the
authored spawn/floor convention, verifies the unchanged four-cell ID64 lamp and
source projection, and uses only a three-point midpoint-ray/rectangle oracle
(expected counts 45/4/3 with exact 1/256 normalization). These are real 1x1 receiver
demands carried on the existing command/frame packets. Capture logs join the
expected DC, actual field DC and same-camera display pixels; they do not clone
the consumer sampler/Hable/MSAA path or qualify it merely because DC agrees.
The three-point diagnostic alone adds no post-Matter-advance, internal-raster
or budget qualification.

Explicit direct-window capture additionally retains a CPU-only initial joint
attachment identity and Driver tick at successful publication. Admission waits
for a current matching attached view whose publication, joint tick and stable
Driver tick have advanced; a legitimate zero-count view remains valid. Reversible
replacement/cancellation preserves the committed baseline, successful publication
replaces it, and teardown clears it. Saved capture proof checks the actual
presented particle identity against that baseline and joins frame/sequence/fence
to the Editor's same-request epoch/count and actual scene-draw raster receipt.
This proves a post-attachment update for the captured frame, without using later
Matter statistics or claiming visual, latency or resource-budget acceptance.

### Actual discovery camera receipts

The existing direct-window +worldgen-report +explicit frame-capture route keeps
CPU-only A/B receipts. A clean presented field/result with exact current matrices,
query rectangle, world and lighting revision authorizes one Game Pan translation
+32 native cells in X at unchanged scale, clamped to world bounds. Actors are not
repositioned. Camera revisions are diagnostic: they advance per model update and
reset on cuts; matrices and fresh frame/cohort stamps identify actual B. A late A
cannot satisfy B. Capture is armed only after a fresh matching B and actual query
window replacement; current post-Matter identity/internal 4x raster proof remains.
Request-to-B-readiness/publication and request-to-fresh-present are reported before
readback/file I/O. Cold setup age is report-only; late warm B is captured with an
explicit 1s NO_GO, never a moment/indirect or performance acceptance. CPU receipts
reset on joint world publication and teardown, survive reversible cancellation
with the committed world, and own no GPU resources. Actual 4096 execution remains
pending; no appearance, albedo, lamp, exposure or source variation is introduced.

`--world-lighting=single-bounce-window` is a distinct typed opt-in beside
`legacy` and `direct-window`; defaults and direct-only behavior remain. It
reuses static-terrain ordinary generated-world startup, BLACK boundary sky and
the existing `--worldgen-report --direct-window-frame-capture=...` A/B route.
No scattering, particle optical sources, recursive feedback or default promotion
is claimed. CLI acceptance/exclusions remain in the existing game_config leaf
`lighting_showcase_is_explicit_interactive_and_never_auto_qualified`.

## Capture-only committed matter census

The explicit direct-window measurement capture retains a Game-owned immutable
physical ledger/catalog and exact staged particle identity/epoch per in-flight
frame. Settlement erases every input, including dropped/error frames. The clean
captured result must join that input, accepted fence and actual presented view;
Game groups authoritative double volumes by native cell/original material/status
and writes a sibling `.matter-census.csv` plus cohort/byte/class summary. The census
includes offscreen rows and declared unavailable/vacuum/opaque/absorbing materials;
it performs no optical solve and establishes no quality or cost acceptance.

Census ledger admission and aggregation additionally require the exact immutable
metadata resource owner, not only numeric cohort/IDs/materials. Capture inputs
retain that owner; the physical ledger/result carry weak identity tokens. A missing
captured catalog ID rejects the receipt, while a present definition without transport
is reported as unavailable. Projection-vector capacity bytes are one payload only;
result transport can copy it, so peak CPU allocation remains unmeasured.


### Actual optical Matter epochs in the existing capture route

Single-bounce-window continues normal simulation and coalesces optical updates.
The actual A/B camera route pins a first nonprovisional incorporated source and
requires a later source with positive GPU changed-cell evidence against a source
already proved in a completed/presented field. Live draw publication/joint tick is
reported separately. First matching B age is fixed before history/readback/file
completion. Those first-B receipts/age verdicts stay immutable. A separate
capture witness may follow a later completed epoch at the same B camera, including
a fresh fully covered old field while newer work is pending. Replacements reset
only candidate history proof; final pixels join their own actual bound field,
accepted temporal publication/history, and fence. Optical updates are never held.
Request-observation age is a lower bound on full edit latency,
whose gate remains unmeasured. No visual/material/exposure change accompanies
this integration.

The explicit lighting showcase service-bay transaction also relocates the amber lamp
between native slots x30..31 and x37..38 at y16..17. Candidate cells, immutable
optical labels and committed typed position share the exact activation revision.
The panel exposes a pending-disabled Move lamp action with destination label;
the existing lamp-check route captures ON/OFF/ON, forward movement and reverse
movement with matching-present timing receipts.

The editable service-bay scene accepts `--lighting-showcase` together with
`--world-lighting=temporal-cascades`. Bootstrap, door changes, lamp switching
and reversible lamp moves send the selected transport and quality settings
through the same world-command transaction. A temporal edit acknowledges the
driver manifest only after presentation of its committed optical revision with
a current nonempty receiver coverage certificate. The existing showcase door
and lamp checks use the same input intents and record input-to-present time.

Temporal game lighting uses the measured R8UI empty-block table by default.
Use `--world-rc-skip-table=false` for the original vacuum-hierarchy control.


The temporal gameplay illumination observation samples the controlled entity's
position in the CPU-owned snapshot of the presented world field. It records
mean incident RGB, transport version, fence and request-to-consumption time.
The diagnostic direct transport modes retain their bounded local terrain ray.


The generated-world temporal diagnostic `--world-rc-distant-query-check`
keeps requesting a 32-cell gameplay window in the opposite map quadrant from
the actor while the camera stays in play. It uses the ordinary field
request, full parent closure, fenced row readback and signed gameplay sample.
Game records exact joint Matter publication identities at driver update and
at sampling, the visible and requested window origins, and their separation.
After at least 1800 committed ticks it pauses the driver and exits on the
matching final field and presentation. Compare coalesced commits to the first
subsequent consumed publication of the same world and terrain. No trace layer
or driver timing overrides are needed.

Temporal worlds may start at a solar hour with `--world-rc-sky-hour=0..23`.
The default uses the authored boundary (black in the playable worlds).
The application advances the hour using the colony's simulation time,
including pause. `--world-rc-day-seconds` sets the duration of a day in
simulation seconds (24–86400, default 86400). The game submits a discrete
hour, logs changes and joins consumption against the actual presented field,
including any explicitly requested gameplay window. A service-bay terrain or
lamp transaction uses the current hour.
