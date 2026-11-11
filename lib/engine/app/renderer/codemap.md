# lib/engine/app/renderer

## Responsibility

The `engine.app:renderer` module provides the content-free generic `Renderer` (surface registry + graphics
queue + validated submission only; multi-surface capable, single main window in production). Scene content lives in `engine.app:renderer.triangle_pass` (`TrianglePass`) and `engine.app:renderer.grid_pass` (`GridPass`, chunk-sized tile quads over the cell world); `engine.app:renderer.presentation` supplies checked W/P/R/S/B values and `ResolvedHdrView`, and `engine.app:renderer.post_process_pass` (`PostProcessPass`) owns exposure adaptation, ceil-sized bloom, the single AgX treatment and fixed-P presentation.
Post-processing runs at the resolved content extent after the scene image; final
nearest enlargement, cropping and the single sRGB decision live in
`present_scene.slang` / `PostProcessPass::present`, and native UI remains last.
Pixelization is a rendering-resolution control (P1..8, default 3), not a
post-effect filter; there is no AA selector or legacy runtime alternative.

`PostProcessPass` consumes an immutable resolved scene-linear HDR view with
strong allocation ownership (`ResolvedHdrView{view, world, backing, valid}`).
`prepare` validates the view against the selected R-sized output, retains it
through stage-B completion, and encodes metering/adaptation/bloom. `render`
encodes the once-only attributed Filament AgX Punchy treatment into its own
R-sized output; `present` samples exact integer resolved pixels. Exposure and
bloom settings never reset reconstruction history; accepted stage-B submissions
commit the exposure role and the active-time baseline, while rejected attempts
commit neither. Target format selects one sRGB encoding (hardware sRGB or shader
UNorm), while float targets remain display-linear for readback.

The earlier Hable spatial prerequisite remains qualified in `spatial_msaa4.slang`.
`engine.app:renderer.reconstruction_pass` now owns the always-on temporal Hable reconstruction:
`ReconstructionPass` allocates the Sx by Sy RGBA16F 4-sample scene target and the
Bx by By single-sample history pair, supplies the padded/jittered `CameraSnapshot`, reconstructs
the quarter-pixel lattice over B, and exposes only the R valid rectangle to post-processing.
`beginFrame` proposes resources and phase without committing; `notifySceneSubmitted` pins stage-A
scene writes; `notifySubmitted` commits camera/grid/cut/history role/phase. Camera cuts, world
replacement and W/P/R/S/B changes reset screen history; pan/zoom reprojects; exposure, bloom and
sky edits preserve it. There is no reactive image, velocity field or alternate AA mode. The
`assets/shaders/hable_temporal.slang` kernel is the temporal article's algorithm, not the spatial
snippet.

Camera-free RHI-facing submission shapes (`RenderPipelineSignature/Key`, `DrawContext/Callback/Submission`,
`ColorAttachmentOps`, `ESurfaceDepthPolicy`, `SurfaceRenderPass`) live header-only in
`engine.app:renderer.types`. There is no `App.Renderer.Types.cpp` — the partition is fully inline in
`App.Renderer.Types.cppm`.

`PreRenderContext/Callback` is the optional synchronous preparation seam on
surface passes and offscreen rendering. It borrows the same command encoder
before the render pass begins, with the frame completion value and fence-wait
hook. Preparation failure abandons the frame before submission; the surface
path still settles its acquired image. Content passes own compute work and
resource transitions issued through this camera-free seam.

Grid staged frames carry owned catalog visuals and an explicit cell metric.
Supported scales are one metre for legacy fixtures and one third metre for
prepared worlds. CPU reference culling, GPU visibility, and vertex positions
all apply that same scale; local atlas addressing stays in fine cells. Ordinary
shutdown resets the metric for reuse. Catalog images and GPU visual resources publish with the owned world candidate.
Terrain admission validates every mip's tight pitch and contiguous owned storage,
including the complete chain in the decoded-byte budget. Upload retains all levels
for trilinear filtering.

Grid payload tables are zero-based. The vertex entry uses visible-ID indirection
or its instance ID directly; it has no per-draw scalar uniform. This keeps the
fragment sampler handle as the terrain program's only entry-point ordinary-data
range and avoids duplicate all-stage push-constant ranges on the current Vulkan
backend. CPU reference and GPU-driven lanes share the same shader.

The normal GPU lane retains its resident table until submissions or cached
content change. Each completion-selected payload slot receives that version
once; camera motion updates frame constants and visibility outputs. Reference
draws invalidate shared slots they overwrite, and buffer replacement invalidates
all slot uploads. Resident-table build/upload counters expose this work.

Interactive atlas publication uses at most 2048 physical chunk slots (64 MiB).
Published slots and retained-table references stay pinned; retired slots become
reusable only after their last submitted reader fence completes. Upload batches
copy at most eight chunks by default and publish complete cohorts together with
the resident table. Deferral retains the previous terrain and pending edit
acknowledgments. Identical retries preserve partial progress. Reference fixtures,
smoke and frozen headless capture drive this same bounded publication path and
refresh renderer fence observations after explicit boundary waits. The synchronous
GridPass preparation API is retired; its historical internal-serialization telemetry
is zero by construction, without mutable counter state. External diagnostic-host
waits are separate and are not represented by that compatibility field.

World-specific terrain, atlas mappings and retained draw state live in one
`GridPass::WorldResources` value. Owner-lane candidate staging/preparation keeps
the active value intact; activation requires matching job, world-generation and
content identities. At most active+candidate or active+retiring owners exist.
The previous owner remains until its last reader, deferred pipeline roots and
completed cached root bindings retire. Completion-safe roots are recreated
lazily; unfinished active roots are retained. Cancellation never activates a
candidate. Normal game transport uses independent world controls. Each world
owns its palette, geometry, atlas and lighting cohort independently. The sealed
ordered terrain texture inventory owns the existing bindless cache, descriptor
buffer, handles, pinned images and slot mapping as one unit. On the first
candidate advance, exact same-device, immutable backing/view, full mip/pitch,
format/interpretation and ordered identity permits a strong inventory lease;
otherwise preparation retains the private one-image budget. Fingerprints and
equal bytes in a different backing allocation never establish reuse. A sealed
inventory cannot acquire new slots or be mutated by another world.
After completion, retirement releases at most eight combined terrain/diagnostic
chunk blocks, one uploaded texture reference and one source image per service
step for the final inventory owner. A nonfinal lease is dropped in constant
ownership work after the same reader/root proof, without draining shared
handles or images. It detaches completed roots once and keeps candidate admission
blocked until disposal finishes. The final fixed number of GPU-object releases remains
on the render owner; shutdown/device-loss epoch resets discard old cache handles.
`GridPassObservations` counts shared inventory storage once across active,
candidate and retiring worlds. Its cumulative comparisons, reuse hits, completed
new texture uploads/source bytes and candidate advance calls are separate from
those live counts; upload bytes exclude descriptor/palette buffers and driver

The former `engine.app:renderer.lighting` partition (CPU optical contracts, slab
interval oracle, hierarchy/DDA and RC estimator) was removed with the superseded
world-lighting integration. GridPass retains only scalar material validation,
filtered texture means, texture residency, normal decoding, texture bombing and
particle admission.

`engine.app:renderer.world_lighting` is the current lightweight record-codec
partition (not the removed optical oracle). It defines the byte-exact published
records `PackedInterval` (stride 8), `PackedCH` (stride 32) and `SourceMaterial`
(stride 8), the decoded CPU values `Interval`/`CH`, the checked codecs
`packCell`, `packRGB9E5`/`unpackRGB9E5`, `packInterval`/`unpackInterval`,
`packCH`/`unpackCH`, `makeSourceMaterial` and `lastTraceFrame`, the approved
coarse-cascade geometry (`cascadeSpacing`, `cascadeDirections`,
`cascadeProbeEdge`, `cascadeGroupEdge`, and `cascadeIntervalStart`, whose
`kCascadePenumbraWidth` derivation yields 1.024..262.144 cells), and the
5x1,048,576-record/40 MiB interval store, 4 MiB RGB9E5 source-cache and
512-square/8 MiB fine-CH constants, plus the committed thermal table shape
constants. The fixed four-frame schedule, atlas addressing and terminal sky
derivation live in their shaders; `lastTraceFrame` is the retained-direction
interface fine reconstruction consumes. It has no device or pass ownership. The shader
mirrors are `assets/shaders/world_lighting_codec.slang` and
`assets/shaders/thermal_emission.slang`; the schedule and sky live in
`assets/shaders/world_lighting_cascade.slang` and
`assets/shaders/world_lighting_sky.slang`.

`engine.app:renderer.world_lighting_pass` is the per-world lighting owner
(`WorldLightingPass`, `WorldLightingFrame`, `WorldLightingInputs`). One instance
lives in `GridPass::WorldResources` with the world's resolved cells, reusable
winner scratch and status tickets; GridPass exposes
`prepareWorldLighting`/`finalizeWorldLighting`/`bindWorldLighting` and the
submission/collection forwards, and no renderer or global service owns lighting
state. `prepare` retains the exact `lighting::WorldLightingInputs` owners,
allocates one 4 MiB resolved-cell grid and two 4 MiB uint32 scratch grids
(`best_key`, `best_row`), then encodes four entries of
`assets/shaders/world_lighting_projection.slang` with UAV barriers between them:
`clearWinners` clears both grids, `reduceKeys` atomic-mins the class/distance key
of every enabled moving solid/liquid particle, `reduceRows` atomic-mins the
metadata-row rank of exact-key candidates, and `resolveCells` overwrites every
cell with terrain, the selected particle's `packCell` material/temperature word,
or zero. Gas and terrain-occupied cells never nominate; the class bit places
transport surfaces ahead of receive-only transparency; the shared coordinate/key
helper gives both reductions bit-identical binary32 squared distances, and the
strictly increasing metadata order resolves exact ties by stable ID. Native cell
coordinates come from committed hot64 positions minus the published origin; the
exact closed maximum endpoint belongs to cell 1023 and nonfinite/outside
positions are rejected (never clamped).

`App.Renderer.WorldLightingPass.Transport.cpp` then rebuilds the full domain
with two dispatches over the resolved cells and the admitted visible palette:
`buildVisibility` (32x32 groups, 256 threads) owns one 32x32 native tile per
group, ORs occupancy into 160 bytes of group-shared storage and writes the
three levels of one 21,504-record structured uint2 buffer (stride 8, 172,032
bytes, level offsets `{0,16384,20480}`, pitches `{128,64,32}`); `buildJump`
(64x64 groups) reads at most three masks per native cell and writes one
`R8Uint` texel. Both dispatches run over reused per-attempt-slot allocations
with ordered resolved-write -> visibility-read -> jump-write -> trace-read
dependencies and no host wait, per-frame allocation or global reduction. The
`assets/shaders/world_lighting_trace.slang` helper traces a finite segment with
slab clipping, aligned-block jump advance, resolved classification and the
cached per-cell source; nonfinite face parameters or a non-progressing
traversal report the numeric status failure instead of a clear interval, and
`traceAdvanceAxis` rebuilds the entered cell from the original ray so a
corner/block-boundary crossing always has its next face ahead.

`App.Renderer.WorldLightingPass.Transport.cpp` then prepares the cached source
and transports the coarse cascades. One ordered 4 MiB RGB9E5 word per native
cell is `authored_emission + thermal_emission(temperature) + diffuse *
previous_angular_mean`; the previous mean is the prior fine-CH DC field
bilinearly sampled at `(cell+0.5)/2-0.5` and scaled by `Y0`, with no extra pi,
cosine, normal, roughness or Fresnel factor. Empty, pure-metal-diffuse and
receive-only/gas cells contribute exactly zero; the pass binds its own
zero-initialized in-place 8 MiB fine-CH field. The source dispatch is ordered
before tracing and before every fine write.
`updateTerminal` fills L5 over the 512-square quarter-probe domain (16x16
threads): four terminal sub-sectors per stored direction with `D=16,384` trace
to the actual forward map exit (slab clip; exterior clear), closing every path
with the analytic fixed-intensity sky and zero transmittance. A miss returns
the sky-sector mean, a hit the cached source. `updateCascade` fills L4..L1 with
a near segment from `c + d*a_l` to each of the four bilinearly gathered
(clamped) parent probes at `p + d*a_(l+1)` and merges the four retained parent
sectors `4*phi+j` by `L = Lnear + Tnear*Lfar`, `T = Tnear*Tfar`, averaging and
weighting once. Levels share one persistent structured uint2 store (stride 8,
five 8 MiB levels, 40 MiB); unselected intervals and never-initialized probes
retain zero radiance and zero transmittance (never vacuum). Explicit global
barriers order source -> L5 -> L4 -> ... -> L1 and the final SRV transition;
nothing is reallocated per frame and there is no CPU wait or readback.

`encodeFine_` then reconstructs the single in-place 512x512x32-byte fine CH
field (8 MiB, zero-initialized per world) with one 8x8-thread dispatch. Fine
centre `f=(p+0.5)*2` owns the connector box `[f-4,f+4]`; four L1 probes are
gathered at `f/4-0.5` after clamping each index to the L1 domain, before that
index derives its centre, group, validity, trace frame or storage address.
Each of the 16 L1 directions uses the probe's own retained trace frame (the
current frame only for a never-initialized tap), exits the box at `e`, and
projects on `v=normalize(e-f)`. The near segment starts at `f+0.001*v`; the
merge is `L=Lnear+Tnear*Lfar`, `T=Tnear*Tfar` with a never-initialized far
interval `(0,0)`. Samples carry `bilinear_weight*2*pi/16`, are fit to the
orthonormal circular basis (`Y0=1/sqrt(2*pi)`, `cos/sin(m*theta)/sqrt(pi)`,
mean subtracted, Lanczos window `sinc(m*pi/5)` applied once), then all nine
decoded FP32 RGB coefficients are blended by
`1-exp(-2*delta_seconds)` and encoded exactly once. The first field bypasses
the blend. Each invocation reads and overwrites only its own 32-byte record;
the pass has no neighbouring CH read, history copy or ping-pong field. Explicit
barriers order source reads before fine writes and fine writes before scene
readers across successive submitted frames. The accepted submission commits
the phase and active-time baseline; rejected/unsubmitted work commits neither.
`angularMean(float2)` samples the decoded field at a world-cell position with
the same `Y0` normalization for later visible/gas use.

Each numeric stage owns one 32-byte `NumericStatusRecord` in the attempt's
fixed-record status buffer: four projection stages, source preparation,
terminal L5, lower cascades and fine reconstruction; words 0/1 are atomic
minima of the invalid-state and range-error index, words 2..7 stay zero.
`finalize` copies
all records to a mapped readback buffer in the same submission; the pass polls settlement
through `collectCompleted(FenceCompletionValue)` with no host wait, and a
completed ticket reports `state_not_recoverable` (invalid arithmetic/state) or
`result_out_of_range` (representational overflow) with category/stage/index/
accepted-frame/world diagnostics. A status slot is reused only after its
submission completed and collection consumed it; `discardUnsubmitted` releases
definitely-unsubmitted attempts and leaves ambiguous submissions quarantined;
`shutdown` drains and inspects outstanding tickets before reporting success.
`bind` supplies `g_resolved_cells`, `g_fine_ch`, the source-material palette, the
thermal table and `g_numeric_status` to material and inspection shaders. A second
`prepare` while an attempt is armed and not yet
submitted or discarded is rejected with `operation_not_permitted` before it
touches the armed attempt's slot, constants or recorded bindings; callers
settle that attempt with `notifySubmitted` or `discardUnsubmitted` first.

## Design

Resident particle presentation accepts a renderer-neutral layout-v4 view with
joint strong owners of hot64 positions and cold24 volume/RGB rows, immutable
stable-ID-ordered metadata, and an epoch-revocation token. The editor rejects
stale unencoded values before backend work. Prepared slots pin both exact source
owners through the existing frame fence, including world retirement.

`App.Renderer.GridPass.Particles.cpp` owns `grid_particle_prepare.slang`: one pack
dispatch on source changes, then three stable frustum-compaction dispatches per
frame. Scratch is O(N), bounded by the global 1M-particle capacity; numerical
rows never reorder. The only remaining indirect particle draw is the sampled
disk used for gas and explicit legacy phase fixtures. Gas retains cold24/palette
RGB and multiplies it by the current fine-CH angular mean; it alpha-composites
over the already shaded scene without relighting the background, adding an
ambient floor, or contributing authored/thermal transport emission. Phase flags
are derived for every material, including terrain elements that become particles
after digging. Radius in metres remains `cell_size_m * sqrt(volume_cells / pi)`.
Reconstructed solids and liquids are no longer drawn as smooth envelopes: the
world-lighting projection selects one categorical material per native cell and
the resolved-cell terrain draw shades it with the same material path.

Task 11 retired `App.Renderer.GridPass.Fluids.cpp`, `App.Renderer.GridPass.Solids.cpp`,
`grid_liquid_kernel.slang`, `grid_solid_kernel.slang`, `grid_fluid_filter.slang`,
their screen-space attachments, splat/filter/composite pipelines and spatial
variants after checking their callers. Particle publication, pack/cull/scatter,
capture census, source-reader fences and the gas disk draw remain. Before a
resident runtime view exists, owned preview/offline seeds use the same GPU
pipeline after one upload per changed inventory. The chunk cache accelerates
admission/version checks only. Selecting a resident view revokes this seed lease;
stale or failed runtime views cannot select the seed adapter. CPU draw geometry,
cell proxies, owner tables, chunk radix sorting and FP64 admission are retired.
Runtime shader compilation and offscreen native/pixelized post gates remain
validation requirements; user visual acceptance is separate.

- **Renderer** (`App.Renderer.cppm/.cpp`): private config (backend-preferred format, image count 3),
  explicit `Renderer(SurfaceSettings initial_settings = {})` captures startup vsync for new surfaces;
  per-surface vsync changes only through lane-affine `reconfigureWindowSurface(handle, generation, settings)`.
  `SurfaceSettings::m_extent` absent retains current geometry. Creation, resize, and explicit changes converge
  on `updateWindowSurface_` and one `buildSurfaceConfig_` helper. Exact nonzero generation is mandatory for
  controls; missing/stale records are rejected, never recreated by a control or stale claimed packet.
  Failed destructive backend operations explicitly restore the old configuration; failure preserves the
  original error, publishes restoration failure/unavailability through `appliedSurfaceSettingsFor`, and
  quarantines admission if unavailable. Restored configurations also mint a new generation.
  Applied vsync denotes successful backend configuration, not proof of physical display synchronization.
  Vulkan acquisition/presentation can return the integration's `errc::pending`
  for an outdated swapchain. The renderer refreshes it through the same fenced
  configuration path, advances its generation, and leaves that frame unpresented;
  an already accepted submission retains its fence settlement.
  State `FlatMap<WindowHandle, SurfaceRecord>` +
  `m_graphics_queue` + `safe_ptr<IRhiService>` + `m_owner` thread id. `SurfaceRecord` owns `ISurface`, a resize-matched D32Float
  depth texture/view, last `m_extent`, `m_configured`, and a generational `m_generation` minted from
  `m_surface_epoch` on every create/resize/unconfigure (zero-size included). The depth pair is shared only by passes that explicitly
  select `ESurfaceDepthPolicy::renderer_owned`; it is not globally attached to presentation. The renderer is
  render-thread confined like the pass caches: `initialize` captures the owner thread, surface and presentation
  paths fail closed (`operation_not_permitted`) off-thread, and `shutdown` clears the owner.
- P1 fence governance (`App.Renderer.FrameFence.cppm/.cpp`, exported as `engine.app:renderer.frame_fence`):
  the ONE app-owned monotonic `FrameFence` (created at `initialize`, one signalled value per frame SUBMIT via
  `SubmitDesc` signalFences after-execute placement; the value commits ONLY on success — a failed submit
  consumes nothing), a `FrameCreditLedger`
  bounding submitted-but-uncompleted frames to `kMaxOutstandingFrames{2}` (busy admission fails the frame closed),
  a `DeferredReleaseQueue` for last-use-keyed object retirement, and pure-CPU helpers `selectRingSlot` /
  `maxSlotValue` / `slotFenceComplete` / `isSurfaceWorkObsolete`. Every `render*` path admits nonblocking via
  `admitFrame_` BEFORE construction (probe-then-acquire through the non-acquiring `checkFrameAdmission`:
  quarantine → `state_not_recoverable`, terminal → `no_such_device_or_address`, only capacity-busy stays
  `device_or_resource_busy`; terminal rechecked AFTER the sweep so a mid-sweep sentinel still stops admission)
  and settles exactly once via `retireFrame_` on a scope guard: `kSubmitted` values ride the unretired set until
  the fence passes them, `kRejected` (definitely unsubmitted) releases immediately, and `kSubmitFailed`
  (possibly executed) quarantines — admission stops and the credit is held until proven idle or confirmed loss,
  the ambiguous value never committed or reused. `sweepCompleted_` reclaims deferred
  releases plus completed credits at every boundary (suppressed only by the tests-only hold seam).
  Completion is read three ways only — proven-idle
  (successful wait / observed poll), positively-confirmed removal (UINT64_MAX sentinel observed or
  `no_such_device_or_address` failure latching terminal, surfacing `no_such_device_or_address`), or UNKNOWN (any
  other failed wait/query: previous completion stands,
  everything retained, admission parked, error propagates; UNKNOWN at teardown RETAINS every GPU object —
  fence alive, deferred held, surfaces/queue untouched — and returns the error for process exit, where the OS
  reclaims; only proven idle or positively-confirmed removal releases early). `abandon()` and
  `notifyDeviceLost()` are confirmed-removal-only (never proof on the UNKNOWN path).
  `waitFor` propagates its post-wait poll failure instead of success, and
  `retireCompletedFrames` propagates poll errors to the caller. `shutdown()` proves idle via `waitOnHost`
  (UNKNOWN → quarantine + retain + return error; confirmed removal → `abandon()` + release-without-touching-
  device), then sweeps, drains, resets credits/quarantine, and
  tears down the fence; `notifyDeviceLost()` (confirmed removal only) abandons wait-free (never `shutdown()`),
  discards pending retirements/credits, and releases surfaces/queue for restart. Tests-only seams:
  `setInjectedSubmitFailure` (possibly-executed submit) + `setTestHoldSweep` (suppress sweep so real submits
  stay pending for the two-truly-pending proof).
  Observability for Gate 1: `lastSubmittedFenceValue` / `completedFenceValue` / `outstandingFrameCount` /
  `surfaceGenerationFor` / `isSurfaceWorkObsolete` / `retireCompletedFrames` / `waitForFenceValue`, plus the
  tests-only `setInjectedSubmitFailure` seam (simulates a possibly-executed submit failure).
- `initialize(rhi_service)` grabs the Graphics `ICommandQueue` only and captures the owner thread, then creates
  the frame fence and reserves the bounded unretired set (allocation-free steady-state retirement);
  `shutdown()` retain-first-error teardown: off-thread call fails closed (`operation_not_permitted`), else
  `waitOnHost`, sweep + drain + credit reset + fence shutdown, unconfigure every configured surface, clear map/queue/service refs and the owner.
  `waitOnHost()` forwards when a queue exists, else success. RHI-service validity is always checked as
  `m_rhi_service.isValid()`.
- `render(description, render_pass, draws)`: rejects uninitialized queue/service (`not_connected`) and empty/null
  attachment descriptors (`invalid_argument`); `inspectAttachment_` resolves each color/depth view to
  format + mip-aware extent + sample count (null/missing/out-of-range/Undefined/0-sample → `invalid_argument`);
  `validateMatchingAttachment_` requires identical extent + sample count across all attachments; resolve targets
  require MSAA source (>1x), 1x resolve, same extent + format. Builds `RenderPipelineKey` signature, creates a
  command encoder, `beginRenderPass` (null → `io_error`), pushes a purple debug group, derives default
  viewport/scissor from the reference extent, then per `DrawSubmission` inserts a pink debug marker, applies
  `m_viewport/m_scissor` or defaults via `RenderState`, and invokes `m_encode_draws(DrawContext{device, pass,
  pipeline_key, viewport, scissor, target_extent})`; finishes and submits one command buffer.
- `renderToTexture(target, draws, options)`: builds a single color attachment from `target.getDefaultView()` with
  `applyColorAttachmentOps_` (load/store/clear color) and forwards to `render()` with the texture label.
  Offscreen callers must `waitOnHost()` before readback. The submit is fenced like every other path
  (value observable via `lastSubmittedFenceValue`); readback callers use `waitForFenceValue` or the legacy
  `waitOnHost` — completion is GPU-only either way, never a display signal.
- `renderAndPresent(window, passes)` is the single-pass convenience API, plus a generational overload
  `(window, passes, SurfaceIdentity)` carrying the packet's surface claim. Surface control reconciles BEFORE
  acquisition: a drifted extent resizes (minting a fresh generation, revalidating signatures) and the current
  packet proceeds against the surface it just shaped — the same packet is never reconfigured and then rejected
  for the reconfigure. Only a mismatch that predates the frame retires BEFORE acquisition as an explicit
  non-rendering outcome (`resource_unavailable_try_again`, mapped by the Editor to a dropped frame, never an
  app failure); a missing record (first frame) accepts and assigns. Identity-handle mismatch fails closed
  (`invalid_argument`). After acquisition the backend obligation is unconditional — an `AcquiredImageGuard`
  armed immediately post-acquire settles the image exactly once on every path (explicit `settlePresent()` on
  the normal path, best-effort present in the destructor on errors and unwinding, never masking the original
  failure). Post-acquire failures (including acquired/depth
  extent mismatch, which used to return early) record `first_err` and skip encode/submit, but `present()`
  still runs (debug-layer acquire→present pairing; both backends deadlock the next acquire otherwise).
  Submission acceptance is independent of present: the SUBMIT commits its value before presenting, so the
  caller stamps ALL participants off the fence advancement even when present fails. A present failure after a
  successful submit leaves the submission GPU-pending under its fence value until
  completion (or the device-loss teardown path) — never recycled early. It acquires one surface image and renders
  the draws carried by each `SurfaceRenderPass` in order before one present. `SurfaceRenderPass` holds its own
  `m_draws` (`std::initializer_list<const DrawSubmission>`) plus its color/additional-color attachments and depth
  policy, so each pass independently selects no depth, the renderer's resize-matched shared depth view
  (`ESurfaceDepthPolicy::renderer_owned`), or an external depth descriptor (`external`). This permits the 3D pass
  to use depth while a following ImGui pass loads color without any depth attachment, or a later compatible pass to
  select a different external view/format without pretending the backend supports different depth views inside one
  native pass. Policy/descriptor mismatches and empty pass lists fail before image acquisition; an acquired
  backbuffer whose extent no longer matches the resize-matched depth view records (`invalid_argument`) and
  becomes present-only. There is no
  `renderAndPresentPasses` and no `SurfaceDrawPass` type. Recording is **two-phase over one shared ScratchPad
  scope**: phase 1 builds every `BuiltSurfacePass` (color attachments, depth attachment, `RenderPassDesc`, then
  `surfaceSignature_` → signature + reference extent) into a reserved `Array`, and the first failure stops the
  build; phase 2 runs only on success, creating one encoder, encoding the already-built passes with the frame's
  polled completion value plus a value-scoped fence-wait hook on every `DrawContext`, then one
  finish + fenced submit — every failure is retained so the acquired image still presents, and a failed pass abandons the
  encoder without finish/submit while `present()` still runs.
- `surfaceSignature_(record, pass, desc, out_signature, out_extent)` is the surface-hoisted signature build. The
  depth policy picks one of two `SurfaceRecord::m_signatures` slots (`none` = 0, `renderer_owned` = 1); external
  depth and any pass with `m_additional_colors` is caller-shaped and never cached. A hit reuses the stored
  signature only when the slot is valid, the depth policy still matches, and the stored extent equals the record's
  current extent (compared componentwise); otherwise `buildRenderSignature_` runs and repopulates the slot.
  `resizeWindowSurface_` clears both slots right after its thread check, so a resize always rebuilds.
- `createWindowSurface_` validates out-param/service/native/handle (plus the X11 display on Linux),
  `createSurface(fromXlibWindow(display, xid))` on Linux and `createSurface(fromHwnd(native))` elsewhere,
  `resizeWindowSurface_` to the current framebuffer size, `try_emplace` (duplicate handle: no clobber,
  transient released, `invalid_argument`; dead path — `renderAndPresent` checks first). `resizeWindowSurface_`: GPU-fences via `waitOnHost` whenever already configured
  (including the minimize path), then zero/negative extent unconfigures and releases depth (keeps the record,
  clears the configured claim even when unconfigure fails so the gate below cannot see a stale claim);
  otherwise creates the D32Float depth texture/view, clears the configured claim, then `configure()` with
  width/height + preferred format + image count + vsync (only success re-sets configured, so a failed
  configure cannot leave a stale claim). `destroyWindowSurface(window)` rejects null handle
  (`invalid_argument`) and forwards to `destroyWindowSurface_(handle)` (unknown handle → `invalid_argument`;
  else move-out, erase, unconfigure). Per-window teardown runs in `ApplicationEditor::shutdown`
  (`destroyWindowSurface(*main_window)` before `destroyWindow` while the renderer is alive);
  `renderer.shutdown()` remains the backstop for any surviving surface (single-window-shutdown-only).
- **Types** (`App.Renderer.Types.cppm`, header-only): `RenderPipelineSignature{array<Format,4> +
  count (kMaxColorFormats) + clamped colorFormats() view, depth_stencil_format optional, sample_count}`
  with count-sensitive `operator==` + `hashValue` combine over the clamped view, memoized as
  `RenderPipelineKey`; `DrawContext{IDevice&, IRenderPassEncoder&, pipeline_key, viewport, scissor,
  target_extent}`;
  `DrawCallback = function_ref<error_code(DrawContext)>`; `TDrawable` concept (`render(DrawContext) →
  error_code`); `DrawSubmission{description, encode_draws, optional viewport/scissor}` with direct and drawable
  (`typeid` label + `nontype<&T::render>`) constructors — retains nothing after `render()` returns;
  `ColorAttachmentOps{clear_color (0.1,0.1,0.2,1), Clear/Store}`;
  `ESurfaceDepthPolicy{none, renderer_owned, external}`;
  `SurfaceRenderPass{surface_color, additional_colors span, depth policy, external depth optional, draws}`;
  `SampleCount{x1=1, x2=2, x4=4, x8=8}` is the closed power-of-two acceptance set (no other value is
  representable), plus `makeSampleCount(u32) -> Expected<SampleCount>` as the single fail-closed u32 mapping
  (non-power-of-two → `invalid_argument`); `RenderPipelineSignature` documents that temporal jitter must never enter it, since a jittered signature would
  invalidate every cached pipeline each frame.
- **TrianglePass** (`App.Renderer.TrianglePass.cppm/.cpp`): pass-owned GPU caches + shared sampler + narrow
  upload APIs (`uploadMesh/uploadTexture/packMaterial/submitInstance`) + a per-frame submitted-instance list +
  pipeline-variant map (kept `CameraSnapshot`). Binds the scalar-handle `mesh_bindless.slang` program
  (StructuredBuffer fetch, no fixed-function geometry). `FrameConstants` keeps its `sizeof == 288` HLSL mirror
  assert (4×float4x4 + 2×float4).
  **One draw path**: `render()` is resolve → plan → upload payloads → `drawInstanced` per group, where
  `planDraws` (static, pure, unit-testable without a device) batches the instances sharing a pipeline variant, a
  resolved buffer pair, and an identical geometry range, and assigns each group a contiguous payload interval.
  Descriptor-backed16B `g_draw.m_payload_base` is bound per group and `startInstanceLocation` is 0, because `SV_InstanceID` is draw-local on
  D3D12, SPIR-V, and Metal alike. Failure is fail-closed but not all-or-nothing: resolve and payload upload are
  all-or-nothing, while encoding stops at the first group that cannot be encoded and prior groups stand.
  There is no indirect or compute-publish lane.
  **Payload ring**: `kPayloadRingSize{3} == kFrameRingSlots` upload buffers at `InstancePayload`
  stride, each created and mapped once in `ensureDirectPayloads_` and kept mapped for its whole life.
  `uploadPayloads_` selects its slot by per-slot completion values (`m_payload_slot_values`,
  `selectSlot_` — stale slots wait instead of overwriting; growth waits for the youngest tracked use via the
  frame's fence hook, R6) and writes through the persistent mapping (no per-frame map/unmap); the submitted
  value is recorded by `notifyFrameSubmitted` (frame owner, post-submit only). Per-variant pipelines carry
  per-slot ROOT objects (`m_slot_roots` + resolved `m_slot_frame_cursors`, bindings fixed at creation);
  a signature change defers every variant through `m_deferred_releases` (reclaimed at each render entry)
  instead of dropping it mid-flight. `encodeGroup_` binds the frame's slot root and resolved cursor.
  `resolveInstances_` scratch is
  GPA-backed by design, never `mem::ScratchPad`: the array grows with submitted instance count while
  ScratchPad has a 32752 B usable-slab ceiling; lifetime is unchanged per-`render()` scope.
  `releaseDirectPayloads_(device)` drops the
  whole ring — unmap only when a device is supplied (growth/replacement), `nullptr` from `shutdown()`/
  `notifyDeviceLost()`, which is safe because `SLANG_RHI_DEBUG_ENABLE_BUFFER_MAP_VALIDATION` defaults to 0.
  `pipelineFor_` validates the signature shape (one color format — the sample count is a `SampleCount`, so every
  value is accepted by construction) BEFORE the cache clear,
  so an unsupported signature can never drop the pipelines built for the last good one.
- **GridPass** (`App.Renderer.GridPass.cppm/.cpp`): content-specific pass beside `TrianglePass` (NOT under `scene/`: it consumes `DrawContext`/`RenderPipelineSignature` from `:renderer.types`, and a `:scene.grid_pass` would invert the established renderer→scene edge). Chunk-sized tiles (1 tile = 1 chunk over the 32×32-chunk × 128²-cell world, never a second tiling system) drawn as flat tinted quads or optional cell materials (no lighting/texturing/GI) from `grid_tiles.slang` (`vertexGridMain` + `fragmentGridMain`, per-draw `uniform uint g_tile_base`, `ConstantBuffer<GridFrame>` 80 B + `StructuredBuffer<GridTilePayload>` 32 B stride with CPU `sizeof/alignof/offsetof` asserts). Plain-data boundary (`GridTileSubmission{dirtyMask,tileRange,chunkId,materialId}`, `GridUploadRequest{chunkId,tileDataView,cellMaterials}` — no sim types; `game/colony` translates grid → submissions). Pure-CPU `planTiles(snapshot, submissions)` culls via epsilon-expanded (`0.5` cell) tile boxes against `snapshot.m_frustum` (edge-touching visible, off-screen/quiescent skipped, submission order kept); staging + dirty-upload lane + cache probe are pure CPU (device-free, render-thread confined), while pre-render cell publication and both render lanes touch the GPU. `render` retains plan → resolve cache-hit-else-stage-cold → completion-selected payload-ring slot (shared cursor covers the indirect-args ring once) → one instanced draw, 6 verts/quad, `startInstanceLocation` 0. Per-slot pipeline roots replace the persistent-root mutation; signature changes defer through the pass-owned release queue (reclaimed each render entry, drained at shutdown/device-loss). Ring growth waits for the youngest tracked use (R6, counted); the cell atlas keeps the existing global-idle wait as first implementation (dirty frames serialize — counted, no overlap claimed). Upload cache clamps at 1024 entries (one per world chunk, `no_buffer_space` overflow). Teardown mirrors `TrianglePass` (stop submissions → drop pipeline root → clear CPU cache → program/ring release, retain-first-error; device loss retains CPU records but invalidates GPU cell residency).
  An additive MDI lane (`compactIndirect` + `renderIndirect`) compacts the same plan to one `IndirectDrawArguments` per visible tile (`{6, 1, 0, plan_index}` in plan order, clamped at the tile-cache budget with `no_buffer_space` overflow, empty plan → zero args and the encode returns before any pass call) and issues one `drawIndirect` over a 3-slot args ring (`Upload`/`IndirectArgument`, same shared cursor as the payload ring) with `g_tile_base` 0; the CPU-staged `drawInstanced` path is retained, the RHI call surface is the existing `IRenderPassEncoder::drawIndirect` (no new RHI entry point).
- **GridPass GPU visibility:** production editor rendering uses `prepareGpuVisibility`
  then `renderGpuIndirect`. Per-slot device-local visible-ID/argument buffers and
  compute roots share the existing completion-governed payload-ring cursor.
  Reset and cull dispatches use `grid_visibility.slang`, with explicit UAV
  barriers and SRV/indirect transitions before one fixed indirect draw. An empty
  resident set resets its instance count to zero. IDs are bounded to 1024; CPU
  reference draw lanes remain available. All draw lanes take geometry bounds
  from the current submission, including cache hits. Production atlas readiness
  covers all submitted dirty chunks; versioned atlas slots publish complete cohorts.
- **GridPass per-cell extension:** `GridUploadRequest::m_cell_materials` is an optional borrowed span of exactly
  16384 row-major u16 entries (0xffff transparent); empty retains flat tiles. Cell requests require a valid
  chunk ID and its exact 128x128 chunk rect, even on clean/no-op requests. Dirty requests pack pairs into
  8192 u32 words per chunk in a lazy pass-owned CPU cache (latest wins). `clearCache()` invalidates cached
  tiles and cells independently of `clearTiles()` staging; regeneration calls clearCache before resubmission.
  Before entering a render callback, owners advance `tryPrepareCellUploads`
  with the renderer's refreshed completion value. Incomplete cohorts retain the
  previous table and reject stale pending draws. Slots remain pinned by resident
  tables and reader fences; the pass never invokes a queue-idle callback. A valid
  4-byte dummy SRV serves flat-only rendering; direct and MDI lanes bind full ranges.
  `GridTilePayload` retains its 32-byte stride; offsets 20/24/28 carry cell-word base/flags/pad.
  MDI `SV_InstanceID`/startInstanceLocation parity remains to be proven with GPU readback on each backend.
- **GridPass categorical PBR:** the visible material entry reads the
  world-lighting owner's resolved cells, fine CH, source palette and thermal
  table. Each raster sample reconstructs its own world-plane position, selects
  exactly one native packed cell, and discards empty samples, so MSAA internal
  occupancy boundaries receive real per-sample coverage. Continuous world-UV
  derivatives are captured before cell selection, and the categorical branch
  returns before the retired contour/cell-word/material selection. Visible
  response uses linear texture RGB times linear tint, authored
  metallic/roughness/IOR (publication-validated, not shader-repaired), mapped
  world normal, `V` = `-forward` for orthographic and `normalize(origin-sample)`
  for perspective, `diffuseIrradiance`/`specularRadiance`, the shared RG16F BRDF
  pair and one authored+thermal emission term; receive-only transparent cells
  omit thermal, use premultiplied authored opacity (`tint.a*albedo.a*opacity`)
  and do not attenuate CH transport. Only a temporary explicit legacy
  flat/contour fallback remains for worlds without published resolved cells;
  Task 14/15 owns its removal once all rendered worlds publish the categorical
  field.
- **GridPass BRDF table:** one immutable 64x128 RG16F table (32 KiB) is
  generated once per GridPass device lifetime at `initialize` by
  `material_brdf_lut.slang` (1,024 golden-ratio/radical-inverse VNDF/Smith
  samples per texel, alpha=max(r,0.001), texel centers
  `(x+0.5)/64,(y+0.5)/128`), transitioned to shader-resource state, and owned
  beside the thermal table. The one-shot generation program and pipeline are
  released immediately after the boot dispatch. The table is released by
  `shutdown`/device-loss after renderer completion and before RHI shutdown; no
  singleton owns it.
- **Presentation** (`App.Renderer.Presentation.cppm/.cpp`): `PresentationGrid{window, resolved, raster, backing,
  pixel_scale}`, checked `makePresentationGrid(framebuffer, P)` W/P/R/S/B arithmetic (positive extents, P1..8,
  overflow rejection; zero framebuffer is caller suspension), immutable `ResolvedHdrView`, finite
  `PostProcessSettings` and `validatePostProcessSettings` (finite fields, EV clamp [-16,16], positive tau,
  ordered auto limits, non-negative bloom controls).
- **PostProcessPass** (`App.Renderer.PostProcessPass.cppm/.cpp`): content-free resolved-size pass beside
  `GridPass`/`TrianglePass`, with no scene callback, AA field or camera parameter. `beginFrame(device, R, world,
  completed)` selects and allocates a 3-slot completion-governed output cohort before `outputTexture()` is
  evaluated and commits no exposure; exhaustion reports `device_or_resource_busy` without a host wait.
  `prepare` validates/retains the resolved view and encodes the 64-bin [-8,8] luminance histogram
  (min(R,256) integer blocks, fractional 10-90% trim), adaptive exposure (per-slot 1x1 offsets, previous role
  committed only on acceptance, world replacement restarts at 0) and bloom (ceil(R/2) level 0, ceil-halved
  separate unweighted images to 1x1 or six levels, edge-replicated 2x2 boxes, then a weighted descent
  `w_l*raw_l + tent(result_(l+1))` with each weight applied once, linear-clamped [1,2,1]/4 tent). `render` combines scene + `intensity*bloom0` and invokes AgX once into its own
  RGBA16F output. `present` loads exact integer resolved pixels at fixed P and encodes sRGB once. Per-slot roots
  and cohorts are keyed by completion; `notifySubmitted`, `notifyPresented`, `discardUnsubmitted` and
  `collectCompleted` provide stage-B/C settlement without steady-state waits.

- **GpuCaches** (`App.Renderer.GpuCaches.cppm/.cpp`): `TriangleBagCache` (vertex-type-agnostic bump buckets,
  stable `TriangleBagRange`, **no dedup** — one range per upload), `BindlessTextureCache` (**content-hash dedup
  + refcount + pin-while-held**, heap slot 0 pinned to the white fallback and host slots start at 1),
  `BindlessMaterialCache` (stable `GpuMaterial` slots, 80 B stride, **slots start at 0 so slot 0 is a real
  material and there is no material fallback entry**), `buildGpuMaterial` pack mapping, pipeline-variant key.
  The bag cache's only draw-facing resolve is `resolveForDraw(handle) -> Expected<ResolvedBag>`, which returns
  the range TOGETHER with its vertex/index buffers; bucket identity is cache-internal. Render-thread confined;
  shutdown order is caches → shared sampler → pipelines before renderer `waitOnHost`.
- Residency/receipt/single-load rules (`TrianglePass` + `GpuCaches`): `uploadScene`
  issues one receipt nonce per scene and `releaseScene` consumes it first, so copies
  share the receipt and repeats fail closed (`invalid_argument`) without decrementing
  a surviving scene's refcounted texture entry. `UploadedScene` carries the mesh-major join table
  (`m_mesh_prim_base`, one base per mesh so prims of mesh i occupy `m_primitives[base[i] ..]`
  contiguously); the static pure-CPU `joinInstancePrim(scene, uploaded, instance, prim_slot)` resolves
  the (mesh, prim) bag with the instance material override applied, fail-closed (`invalid_argument`)
  on mesh/table/slot mismatch. Material slots never recycle and
  texture heap slot 0 stays pinned to the white fallback; capacity overflow is
  deterministic fail-closed (`no_buffer_space`), never a partial upload.
- `hashValue(TrianglePipelineVariant)` seeds the trivial hash with
  `hash::default_seed_v`, so variant keys are deterministic across runs (stable
  pipeline-cache lookup, no seed-0 degenerate combine).
- `initialize(rhi, shader, content_dir)`: the single bindless program + `caches.initialize(device)` with
  reverse-order rollback; `createShaderProgram_` loads `mesh_bindless.slang` and links `vertexIndirectMain` +
  `fragmentMain`; `pipelineFor_` keys opaque/mask variants (blend rejected) and drops the whole cache on a target
  signature change; `update(dt, camera_view)` caches the snapshot; `render(ctx)` encodes one instanced draw per
  planned group.
  Linux/clang bring-up: the single `setRenderState` literal in `TrianglePass::render` sets `.indexBuffer={}`
  (App.Renderer.cpp:248 is a forwarder, not a literal); Clang via -Wextra+-Werror (Clang.cmake has only
  -Wall/-Wextra + -Werror, no explicit -Werror=missing-field-initializers flag) rejects the omission MSVC
  zero-inits; bindless NON-INDEXED fetch uses no index buffer.

## Flow

1. Startup → `renderer.initialize(rhi_service)` (graphics queue only); first `renderAndPresent(window, …)`
   lazily creates + configures the window surface from `Window::m_framebuffer_size/m_native`
2. Per-frame → stack the scene and UI `DrawSubmission` arrays → two `SurfaceRenderPass` records →
   `renderAndPresent` (resize on drift → acquire → depth-free post presentation → color-load/no-depth UI render
   pass → present). The temporary cleanup path renders stage A into an editor-owned single-sample R-sized scene
   image, encodes metering/adaptation/bloom and the AgX output in the pre-render seam, then presents the
   display-linear result and native UI in the same submission; Task 12 replaces the scene image with
   ReconstructionPass and Task 14 owns the three-submission split.
3. Offscreen/tests → `renderToTexture(target, …)` → `waitOnHost()` → readback
4. On minimize/resize-to-zero → surface unconfigures but the record stays; next non-zero frame reconfigures
5. Shutdown → Editor proves GPU idle first (`waitOnHost`; UNKNOWN → quarantine + retain + return error for
   process exit where the OS reclaims; confirmed loss → `notifyDeviceLost` everywhere,
   surface destroy skipped) → `triangle.shutdown()` + `grid.shutdown()` + `post_process.shutdown()` (safe: nothing pending) →
   `destroyWindowSurface(*main_window)` → window destroyed via the window
   service → `renderer.shutdown()` (wait, unconfigure any survivor, release)

## Integration

- **Consumers**: owning application shell (owns `Renderer` + `TrianglePass` + `GridPass` + `PostProcessPass`,
  drives update/render/shutdown)
- **Depends on**: `engine.core`, `engine.math`, `engine.rhi` (devices, queues, surfaces, passes, pipelines),
  `engine.shader` (per-pass program loads), `engine.image` + `engine.mesh` (cache upload types only),
  `:service.window` + `:window.handle` (Window resolves surfaces),
  `:scene.camera` (TrianglePass `CameraSnapshot` only — never a mutable `Camera`)
- **Provides**: `engine.app:renderer`, `engine.app:renderer.frame_fence`, `engine.app:renderer.gpu_caches`, `engine.app:renderer.triangle_pass`, `engine.app:renderer.grid_pass`, `engine.app:renderer.presentation`, `engine.app:renderer.post_process_pass`, `engine.app:renderer.reconstruction_pass`, `engine.app:renderer.types`, `engine.app:renderer.world_lighting`, `engine.app:renderer.world_lighting_pass`
  (camera-free boundary: passes consume `DrawContext`/snapshot data while drawing)

## Key Files

- `App.Renderer.cppm` — generic `Renderer` declaration (config, surface registry incl. generations, render/renderToTexture/renderAndPresent/waitOnHost/destroyWindowSurface, fence/credit/retirement API, device-loss hook)
- `App.Renderer.cpp` — Renderer implementations (attachment inspection/validation, fenced encode/submit, surface create/resize/destroy, generational rejection, unconditional-present path, retain-first-error shutdown)
- `App.Renderer.FrameFence.cppm/.cpp` — NEW P1 fence-governance partition (`FrameFence`, `FrameCreditLedger`, `DeferredReleaseQueue`, `selectRingSlot`/`maxSlotValue`/`slotFenceComplete`/`isSurfaceWorkObsolete`, 2-credit bound)
- `App.Renderer.TrianglePass.cppm` — `TrianglePass` declaration (FrameConstants layout, snapshot cache, caches, pipeline helpers)
- `App.Renderer.TrianglePass.cpp` — TrianglePass implementations (invariant state, shader program, variant pipelines, resolve/plan/upload/encode, frame-constant upload)
- `App.Renderer.GridPass.cppm` — `GridPass` declaration (boundary structs, tile payload mirror, snapshot cache, CPU tile cache, pipeline helper)
- `App.Renderer.GridPass.cpp` — GridPass implementations (validation/planTiles, shader program, single pipeline, upload lane, ring upload, single-draw encode)
- `App.Renderer.Presentation.cppm/.cpp` — presentation grid math, `ResolvedHdrView`, post settings and validation
- `App.Renderer.PostProcessPass.cppm/.cpp` — independent post owner: output cohorts, metering/adaptation, ceil bloom, once-only AgX, fixed-P presentation and settlement
- `App.Renderer.ReconstructionPass.cppm/.cpp` — always-on Hable temporal owner: padded 4x scene target, resolved history pair, padded/jittered camera, lattice reconstruction, accepted-history settlement and fenced allocation retirement
- `App.Renderer.GpuCaches.cppm` — pass-owned cache vocabulary (handles, `TriangleBagRange`, `ResolvedBag`, `GpuMaterial`, caches)
- `App.Renderer.GpuCaches.cpp` — cache implementations (uploads, dedup, pack, teardown)
- `App.Renderer.Types.cppm` — boundary types (`RenderPipelineSignature/Key`, `DrawContext/Callback/Submission`, `ColorAttachmentOps`, depth policy, `SurfaceRenderPass`); header-only, no matching `.cpp`
- `App.Renderer.WorldLighting.cppm/.cpp` — published world-lighting record words, CPU interval/CH values, RGB9E5/half/AC codecs, source-material admission and retained-direction scheduled frame identity
- `App.Renderer.WorldLightingPass.cppm/.cpp` — per-world categorical projection owner: resolved cells, two winner-scratch grids, numeric status ring, settlement and diagnostics
- `App.Renderer.WorldLightingPass.Projection.cpp` — `encodeProjection_` clear/reduce/resolve encoding with the shared coordinate/key contract
- `App.Renderer.WorldLightingPass.Transport.cpp` — full-domain visibility/R8 jump rebuild, per-attempt-slot acceleration storage, cached-source preparation, continuously scheduled L5-L1 cascade encoding and the persistent cascade/source/fine-CH storage

The shared contour surface is enabled for catalog sessions. GridPass uploads
`math::contourPoints/contourCases` and completion-safe per-slot global chunk maps;
retained lookup slots, including diagonal neighbors absent from the compact draw
table, stay pinned until the publication and reader-fence rules release them.
Metric, contour mode and boundary material publish with the resident table.
World replacement and deferred edits therefore keep the previous surface's
shape/scale until replacement data is complete. Grid appearance and evolving
contour style are reviewed manually. Automated gates cover admission, resource
lifetime, visibility, publication and GPU timestamps.

GridPass world bundles also own the bindless texture cache, source image pins,
source-to-resident slot mapping and immutable 96-byte terrain visual palette (including per-material roundness).
One pass sampler/fallback and a seven-level boundary-noise texture are shared. Candidate texture preparation is bounded to
one admitted 2048-square RGBA image per update; canceled uploads remain retained
until a subsequent accepted frame fence covers initialization. Coordinator-side
capture produces owned u16 cell uploads without copying full simulation cells.

Opt-in `GridPassObservations` snapshots are captured only on the render owner and
carried by value in the existing frame result. The editor mirrors them for game
acceptance reports. They count encoded GPU terrain draw/dispatch commands, atlas
publication/table work, retained atlases and live per-world texture entries/slots;
they do not assert GPU completion, total driver allocation or total VRAM.

Renderer owns a four-query GPU timestamp pool (two completion-gated pairs).
Timestamps enclose each frame command encoder, including pre-render preparation
and all surface passes, excluding separate uploads and presentation. Collection
requires both the application fence and nonblocking query readiness. Saturated
query storage omits telemetry; query failures disable telemetry with a diagnostic.
The pool follows the same proven-idle/confirmed-removal release rules as other
Renderer resources. Delayed `GpuFrameTiming` observations retain their submission
identity when transported to the main thread in `RenderFrameResult`.

GridPass retains an optional immutable numeric surface profile with each world
bundle. Its background-only 16 KiB height constant buffer selects sky above the
exact fine-column surface and authored parallax below. The 304-byte frame ABI
uses its former padding word for the surface enable flag; tile and particle
frames keep their size. Surface data shares candidate publication and retirement.


The authored material optical bridge appends versioned model/sigma to the existing
`GridTerrainVisual` palette (96 bytes, previous offsets unchanged). Tags 0/0x101/
0x102/0x103 distinguish unavailable/vacuum/opaque/absorbing version1. Extinction is
per world metre; existing linear emissive RGB*strength is source function S,
absorbing derives j=sigma*S, opaque emits S once, and vacuum source/sigma are zero.
Visual-only unavailable rows remain accepted, while strict optical conversion
rejects missing transport. Capture/stage validation and palette equality include
optics; shader and particle-carrier reflection plus actual upload stride checks
cover all same-palette consumers. Dense palette storage is 6 MiB per owner rather
than 5 MiB, with overlap under existing fence retirement. A qualification-only
compute probe in grid_tiles reads the real 96-byte layout without entering draws.
The bridge passed independent source review and focused Debug/Release schema,
numeric/GPU ABI, terrain/particle raster and shipped-profile gates. Debug's
opt-in radiance entry confirmed system Khronos Core and GPU Assisted validation;
the earlier broad asset GPU timeout remains unresolved. Terrain/matter
reconstruction and scene lighting activation remain separate work. The prior deterministic half-storage stage
passed Debug ordinary/real GPUAV and focused Release CPU/GPU gates.


The former `renderer.optical_geometry` partition (analytic Bezier terrain oracle,
`canonicalTerrainRoundnessMean` and parametric optical traversal) was removed
with the superseded world-lighting integration. No C++ symbol from that
partition remains; the retained contour roundness construction is the shared
Slang code in `grid_contour_math.slang`.


## Retired reconstruction route

`RetroSettings`, `RetroPass`, palette quantization, the temporal selector and the
opt-in spatial integration were removed in Task 2. `Resolution` pixelization P1..8
is carried on the owned frame request; `PostProcessPass` has no AA mode or
enable/disable combination. The spatial sample-rate kernel and its Hable
qualification remain the subject of Task 12, not a runtime alternative.

The retained contour roundness construction lives in
`assets/shaders/grid_contour_math.slang` and serves the raster/terrain consumers.
GridPass validates finite nonnegative [0,1] roundness rows before staging; the
deleted optical oracle supplied no retained C++ coefficient, ray-query or
intersection path.


## Opt-in final surface readback

`SurfaceSettings::m_readback_enabled` is a constructor-only capability, default off; Renderer preserves it across resize/vsync surface transactions. An optional owning CPU output on the snapshot `renderAndPresent` overload records one texture-to-buffer copy after all surface passes in the same encoder. The fixed Renderer staging slot distinguishes unsubmitted, unknown-submit and accepted-fence ownership; unknown teardown retains it until idle/removal proof. Presentation settles before the actual fence wait/map/copy/unmap can publish `SurfaceFramePixels`. Only RGBA8/BGRA8 UNORM and sRGB display formats are admitted. Off requests allocate/copy/wait nothing. A 4K diagnostic holds about 33 MB each of GPU staging and CPU bytes; this is not a performance gate.

Readback requests use a portable copy layout with row alignment equal to the maximum of 256 bytes and the validated power-of-two device requirement, a multiple of both the backend alignment and four-byte texels. At 3840 pixels the 15360-byte row is already aligned. CPU storage retains this reported pitch, copies only each row's meaningful pixel bytes, and leaves all row padding/layout tail zero. The optional `image_acquired` output is independent of presentation/readback success and distinguishes proven pre-acquire rejection from every acquired-image fate.
Renderer readback ownership/fates and its fixed staging slot are unchanged by
the client-side capture sequence.

Bloom is no longer an opt-in reconstruction prerequisite. `PostProcessPass`
always owns it (enabled by the game setting): exposure-aware prefilter, ceil-sized
level 0 and ceil-halved separate RGBA16F images to 1x1 or six levels, edge-
replicated 2x2 boxes, once-applied level weights and a linear-clamped tent
upsample. It is independent of AA, pixelization and reconstruction history;
`post_process.slang` adds level 0 once at the resolved extent before AgX.

## Capture-only particle census

`prepareGpuParticles(context, census_world_edge)` uses zero for the unchanged normal
path. Positive diagnostic admission projects every metadata row before pack-cache
and frustum compaction, reading moving Hot64 by sourceRow and gas world positions
directly. Stable/original IDs and half-open native cells/status are never culled or
clamped. Declared vacuum is the existing palette transport tag, independent of phase.
The lazy program and selected slot own projected/readback/root resources and the
full numerical/readonly/metadata lease; existing submission/quarantine/teardown
rules apply. `readParticleCensus` requires that slot's exact stamped accepted fence
and explicit completion before mapping into an owned result. Zero populations are
explicit empty results. Diagnostic buffers join existing particle byte observations;
no normal-frame wait, queue, dense world allocation or optical solver is added.

Census readback accepts only the two fence values; mapping uses the selected slot's
actual allocation device, so a foreign device cannot be supplied. Returned rows
include a weak metadata identity token without taking GPU-resource ownership.


