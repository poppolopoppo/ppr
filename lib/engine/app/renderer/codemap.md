# lib/engine/app/renderer

## Responsibility

The `engine.app:renderer` module provides the content-free generic `Renderer` (surface registry + graphics
queue + validated submission only; multi-surface capable, single main window in production). Scene content lives in `engine.app:renderer.triangle_pass` (`TrianglePass`) and `engine.app:renderer.grid_pass` (`GridPass`, chunk-sized tile quads over the cell world); `engine.app:renderer.retro_pass` (`RetroPass`) owns a fenced offscreen ring with independent pixelization and 32-color palette/dither controls.
Optional temporal supersampling operates at the chosen scene resolution, combining four-phase jitter,
raw 4x MSAA color bounds, camera/depth reprojection and clamped Catmull–Rom RGB history before palette treatment; UI stays native.
Unsupported multisample shader loads retain spatial rendering. History commits only on accepted submits
and resets on cuts, extent/world/policy/projection-mode changes. History alpha stores orthographic
device Z or perspective reciprocal clip W, with a negative background sentinel. During motion every
nonzero color tap in the full 4x4 reconstruction must match the expected class and a single affine
surface-depth fit. Metadata reprojection aligns current/previous jitter phases separately from RGB
motion, avoiding phase-dependent rejection on perspective slopes. Current-frame
influence grows from 5% stationary to 50% at 0.25 internal pixels/frame. Composite parallax backgrounds
render with the unjittered camera and retain a stable cubic spatial fallback during motion.
Center-depth camera reprojection is exact for the shipped orthographic pan/roll/zoom; translated
perspective slopes use the nearest-MSAA-depth center approximation. Object motion uses reactive color
rejection (no object motion buffer).

`RetroSettings` also carries default-off HDR display treatment and finite manual
exposure in [-16,+16] EV through the existing owned frame snapshot. Fallible
`RetroPass::beginFrame` rejects invalid exposure before changing pass state.
Exposure and the attributed Filament AgX Punchy approximation run after the
unexposed linear-HDR history, independently of palette/pixelization; they neither
select an EV pipeline variant nor reset history. Actual target format selects one
sRGB encoding (hardware sRGB or shader UNorm), while FP16 diagnostic output stays
linear. The first milestone uses controlled finite HDR producer fixtures; it is
not ordinary-world activation, Hable TSR or complete display integration.

The selected Hable spatial prerequisite is being qualified separately in
`spatial_msaa4.slang` and the asset sampling/filter fixtures. Its continuous
diamond reconstruction preserves signed linear HDR and exact 2x extents.
The default-off current-frame integration is now authored inside RetroPass and
awaits its own source/execution qualification. The fixture's raster
threshold brackets do not establish a runtime exact sample-pattern contract;
the actual terrain/background sample-frequency variant, temporal history and
4K cost require their own qualification.

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
allocation overhead. The existing shared native WorldRadiance GPU leaf exercises
the 138-record cold/warm path, independent palette rendering, reader cancellation,
admitted identity misses and malformed-pitch rejection on one device/Grid pass.

`engine.app:renderer.lighting` (`App.Renderer.Lighting.cppm/.cpp`) currently owns
CPU-only explicit cell optical contracts, a slow slab-intersection RGB interval
oracle, an immutable vacuum hierarchy/scalar hierarchical DDA and a checked
fixed-lattice RC streaming resource estimator. It has no GPU
allocation or frame integration and no sim dependency. Production contour/matter
mapping, GPU traversal/transport and display integration remain staged.
Behavioral tests live in `App.Lighting.Tests.cpp` under the CPU `app/lighting`
group, registered once in `engine.tests.app.functional_cpu`.

`engine.app:renderer.lighting_pass` (`App.Renderer.LightingPass.cppm/.cpp`)
currently owns FP32 GPU optical trace and complete small-fixture RC qualification
batches and `radiance_cascades.slang`. It admits buffers before allocation, records scalar
hierarchical DDA through `PreRenderContext`, and exposes completion-gated diagnostic
readback. It has explicit submitted/unsubmitted/ambiguous settlement and requires
completed-fence or idle/removal proof before release. RC uses two fixed parity
buffers, parent-bin near origins, four connector compositions before bilinear and
angular weights, boundary-complete top rays and a bounded C0/C1 diagnostic readback.
Its complete-fixture API rejects more than 4096 cells, incomplete tiles and redundant
cascades; this does not limit the estimator or future streamed solver. Each batch
owns flattened uint vacuum flags and 16-byte level metadata. GPU level dispatches
write disjoint ranges with intervening barriers; traversal reads the completed
hierarchy as an SRV and skips only fully in-domain explicit vacuum. Caller edits
require reprepare; partial/odd-axis blocks cannot skip padding. Admission includes
candidate+previous hierarchy resources. This is an independently authored u32 tree,
not a verified v22 bitmask/jump-table port or measured speedup. There is no
gameplay/frame integration. `app/radiance_gpu` owns its GPU-locked
readback/lifecycle tests separately from CPU-only `app/lighting`. The pre-hierarchy
FP32 path passed independent source review and the registered validation entry
with the system Khronos layer plus core/GPU-assisted checks on Vulkan/RTX2080;
the hierarchy stage also passed routine GPU and real-layer core/GPU-assisted
execution (10.82/12.43 s), with one axis block span versus 64 cell spans on 64x64.
This is work evidence only. Physical RC error remains
under qualification (open-room scalar diagnostic about 14.6%); complete-world,
packed storage and other-backend claims are not established. Explicit
`GpuIntervalStorage` selects the existing FP32 default or a qualified bounded
16-byte independent-RGB-half L/T record with per-slot status. A narrow explicit
integer RTE codec replaces native conversion after normal/subnormal probes failed
the desired rounding contract. Codec cost is unmeasured; qualification raw words
support exact bit validation. Packed arithmetic remains FP32; only C0/selected-C1 local span counts use a separately admitted uint
plane. Packed readback is 20 bytes/returned record. Overflow/nonfinite/domain
failures remain errors, with no clamp or automatic fallback. The numeric estimator
remains caller-stride driven; no game/publication integration is added.

Generic-cell GPU qualification explicitly supports zero or normal finite FP32
raw ray components, coefficients and sources, with a positive normal cell metric.
Opaque source admission is shared with terrain qualification because its source
loads have the same permitted flush-to-zero behavior. Nonzero FP32 subnormals
are rejected on upload; binary16 subnormals such as 2^-24 remain normal FP32 and
the deterministic half codec is unchanged. Generated generic connectors require
actual equal endpoints for identity and reject collapsed positive lengths or
directions. Positive absorbing distance, depth, attenuation, cumulative
transmission and each needed source integral must remain normal finite values.
The existing ordered volume, opaque and RC composition/weight products report
status 1 on positive underflow/overflow; exact zero operands, zero extinction,
opaque termination and explicitly quantized half zeros retain their meanings.
The CPU oracle is unchanged. This conservative unsupported-domain policy is a
qualification safety boundary, not a production water discarded-radiance policy.
The unchanged 512-cell sigma .25/32 queries now require visible failure in both
storage formats; the weak long run and a short depth-32 path retain successful
storage-error coverage. This closes silent arithmetic underflow without changing
those queries or admitting opaque-media approximations.
`PPR_TEST_RADIANCE_CASCADE_DIAGNOSTIC=1` enables the fixture's forwarding error
sink, retaining producer ticks/thread and structured ray/status/span fields and
draining before restoring the previous writer at teardown.

## Design

Resident particle presentation accepts a renderer-neutral layout-v4 view with
joint strong owners of hot64 positions and cold24 volume/RGB rows, immutable
stable-ID-ordered metadata, and an epoch-revocation token. The editor rejects
stale unencoded values before backend work. Prepared slots pin both exact source
owners through the existing frame fence, including world retirement.

`App.Renderer.GridPass.Particles.cpp` owns `grid_particle_prepare.slang`: one pack
dispatch on source changes, then three stable frustum-compaction dispatches per
frame. Scratch is O(N), bounded by the global 1M-particle capacity; numerical
rows never reorder. The remaining indirect particle draw keeps sampled disk
alpha for gases and explicit legacy phase fixtures. Reconstructed solids use
their element's exact terrain `materialSurface` path: physical world UVs, albedo,
normal map and optical properties. Gas retains cold24/palette RGB. Phase flags are derived for every
material, including terrain elements that become particles after digging.
Radius in metres remains `cell_size_m * sqrt(volume_cells / pi)`.

`App.Renderer.GridPass.Fluids.cpp` owns the 2D screen-space liquid path. Two
indirect quad splats select the closest element in R32Uint/D32Float and accumulate
same-element coverage in R32Float. `grid_liquid_kernel.slang` shares three-radius
support, amplitude 4/9 and its matching isocontour across splats, optical height
and visibility. The cubic kernel preserves integrated mass and an isolated disk
radius while covering deformed interparticle gaps.
`grid_fluid_filter.slang` applies a bounded non-separable 5x5 material-guided
compute filter using shared-memory tiles. A final fullscreen composite uses
material-aware bilinear reconstruction and a bounded rounded front-height
proxy derived from density. Camera-ray-aware Fresnel, roughness-controlled sky
and soft-key reflections, nearby backdrop reflections, authored opacity/IOR and
emissive properties give liquids a glossy volume in the 2D scene. Refraction
uses the authored physical depth and rounded normal, capped at twelve pixels;
reflected backdrop lookups have an independent twenty-four-pixel bound. The same
terrain contour occlusion and optical backdrop remain authoritative. Foreign element taps are renormalized rather than
treated as empty space, avoiding interface gaps. Per-fragment world-plane
reconstruction supports perspective views as well as orthographic pan/roll/zoom.
This is the classic screen-space method adapted to 2D, without neighbor-PCA
anisotropic particle kernels.

Each existing completion-selected particle slot owns its attachments and roots;
resize replaces only that completed slot. Fields are half the scene viewport,
limited to a 1280-pixel longest edge with preserved aspect ratio, and use
16 bytes per field pixel including element,
depth, raw and filtered fields. Three populated slots cost about 24 MiB at a
1920x1080 scene, about 40 MiB at 3445x1837, and at most 75 MiB at the cap
(excluding driver allocation overhead).
32-bit accumulation remains finite for the admitted 1M overlapping unit kernels.
Resources are allocated only for an inventory containing liquids and follow
world retirement and source-reader fences. Occlusion follows the terrain mode
actually drawn; final particle/composite draws have depth writes disabled.
An optional test-only query pool records six GPU phase pairs; normal production
frames encode no additional timing queries or readback. Tests serialize its
reset/read/replacement against the exact submission completion fence.

`App.Renderer.GridPass.Solids.cpp` owns a separate solid occupancy lane on the
same completion-selected particle slots. World-XY octagonal footprints retain
flat faces under camera movement and roll. `grid_solid_kernel.slang` calibrates
apothem from the physical circle area and shares support/amplitude/isolevel with
splat geometry, composition and conservative visibility. All solid elements
contribute to one R32Float occupancy sum; an independent R32Uint/D32Float nearest
grain selection supplies categorical element labels in mixed rubble. Native
composition filters their bilinear coverage using each distinct label's shaded
premultiplied color, preserving texture interiors and antialiasing interfaces.
Empty labels are excluded without changing the independent occupancy mask.
The solid composite uses coherent world/displaced-contour derivatives, the same
terrain occlusion, and exact `materialSurface` shading. It has no liquid filter,
rounded height or reconstructed optical normal. Draw order is liquid envelope,
solid envelope, then remaining gas disks.

Solid fields use the same aspect-preserving half-resolution/1280 longest-edge
bound, with three attachments totaling 12 bytes per pixel per populated cohort.
Three cohorts add about 30 MiB at 3445x1837, at most 56.25 MiB for a square capped
field, excluding driver allocation overhead. Solid and liquid attachments are
separate because both stay live until composition. Allocation/resize commits a
complete replacement only after that exact particle slot's previous reader is
complete; the enclosing particle source/retirement fences retain both lanes.

Before a resident runtime view exists, owned preview/offline seeds use the same
GPU pipeline after one upload per changed inventory. The chunk cache accelerates
admission/version checks only. Selecting a resident view revokes this seed lease;
stale or failed runtime views cannot select the seed adapter. CPU draw geometry,
cell proxies, owner tables, chunk radix sorting and FP64 admission are retired.
Runtime shader compilation and offscreen native/retro4 pixel gates remain
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
- **RetroPass** (`App.Renderer.RetroPass.cppm/.cpp`): content-free present pass beside `GridPass`/`TrianglePass`.
  `RetroSettings{bool m_enabled; u32 m_pixel_scale{3}}` is session policy; `clampPixelScale` pins 2..8,
  `sceneExtent` floor-divides the native extent per axis with a one-pixel floor, and `scenePipelineSignature`
  is one RGBA16Float color target with D32Float depth at 1x. `prepareScene` selects a 3-slot
  completion-governed target ring, recreates on extent change, clears and opens one offscreen pass, forwards
  the frame's value-scoped fence-wait hook, and invokes the caller's scene encode with the low-resolution
  `DrawContext`; the caller's content passes rebuild their signature-keyed pipelines for that target.
  `render` reclaims deferred releases, caches the present pipeline per surface signature (retiring the old one
  at the last submitted fence value), uploads the 528-byte `RetroParameters` (32 DawnBringer-32 sRGB entries,
  source extent, dither strength), binds the selected slot's color view plus one sampler, and draws one
  fullscreen triangle. `notifyFrameSubmitted` stamps the recorded slot. Render fails closed without a ready
  scene; shutdown/device-loss are wait-free, drain deferred releases, and release the ring targets and roots.
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
   `renderAndPresent` (resize on drift → acquire → depth-enabled 3D render pass, or a retro prepare into the
   low-resolution ring plus a depth-free nearest/palette present → color-load/no-depth UI render pass →
   present); `TrianglePass::update(snapshot)` precedes submission; the scene submission carries
   `TrianglePass::render` after the grid background/GPU lanes, and with retro enabled it encodes inside
   `RetroPass::prepareScene` instead of the surface pass.
3. Offscreen/tests → `renderToTexture(target, …)` → `waitOnHost()` → readback
4. On minimize/resize-to-zero → surface unconfigures but the record stays; next non-zero frame reconfigures
5. Shutdown → Editor proves GPU idle first (`waitOnHost`; UNKNOWN → quarantine + retain + return error for
   process exit where the OS reclaims; confirmed loss → `notifyDeviceLost` everywhere,
   surface destroy skipped) → `triangle.shutdown()` + `grid.shutdown()` + `retro.shutdown()` (safe: nothing pending) →
   `destroyWindowSurface(*main_window)` → window destroyed via the window
   service → `renderer.shutdown()` (wait, unconfigure any survivor, release)

## Integration

- **Consumers**: owning application shell (owns `Renderer` + `TrianglePass` + `GridPass` + `RetroPass`, drives update/render/shutdown)
- **Depends on**: `engine.core`, `engine.math`, `engine.rhi` (devices, queues, surfaces, passes, pipelines),
  `engine.shader` (per-pass program loads), `engine.image` + `engine.mesh` (cache upload types only),
  `:service.window` + `:window.handle` (Window resolves surfaces),
  `:scene.camera` (TrianglePass `CameraSnapshot` only — never a mutable `Camera`)
- **Provides**: `engine.app:renderer`, `engine.app:renderer.frame_fence`, `engine.app:renderer.gpu_caches`, `engine.app:renderer.triangle_pass`, `engine.app:renderer.grid_pass`, `engine.app:renderer.retro_pass`, `engine.app:renderer.types`
  (camera-free boundary: passes consume `DrawContext`/snapshot data while drawing)

## Key Files

- `App.Renderer.cppm` — generic `Renderer` declaration (config, surface registry incl. generations, render/renderToTexture/renderAndPresent/waitOnHost/destroyWindowSurface, fence/credit/retirement API, device-loss hook)
- `App.Renderer.cpp` — Renderer implementations (attachment inspection/validation, fenced encode/submit, surface create/resize/destroy, generational rejection, unconditional-present path, retain-first-error shutdown)
- `App.Renderer.FrameFence.cppm/.cpp` — NEW P1 fence-governance partition (`FrameFence`, `FrameCreditLedger`, `DeferredReleaseQueue`, `selectRingSlot`/`maxSlotValue`/`slotFenceComplete`/`isSurfaceWorkObsolete`, 2-credit bound)
- `App.Renderer.TrianglePass.cppm` — `TrianglePass` declaration (FrameConstants layout, snapshot cache, caches, pipeline helpers)
- `App.Renderer.TrianglePass.cpp` — TrianglePass implementations (invariant state, shader program, variant pipelines, resolve/plan/upload/encode, frame-constant upload)
- `App.Renderer.GridPass.cppm` — `GridPass` declaration (boundary structs, tile payload mirror, snapshot cache, CPU tile cache, pipeline helper)
- `App.Renderer.GridPass.cpp` — GridPass implementations (validation/planTiles, shader program, single pipeline, upload lane, ring upload, single-draw encode)
- `App.Renderer.RetroPass.cppm` — `RetroPass`/`RetroSettings` declaration (pixel-scale/extent/signature helpers, offscreen ring, present API)
- `App.Renderer.RetroPass.cpp` — RetroPass implementations (ring targets, offscreen scene pass, palette upload, present pipeline, deferred release, teardown)
- `App.Renderer.GpuCaches.cppm` — pass-owned cache vocabulary (handles, `TriangleBagRange`, `ResolvedBag`, `GpuMaterial`, caches)
- `App.Renderer.GpuCaches.cpp` — cache implementations (uploads, dedup, pack, teardown)
- `App.Renderer.Types.cppm` — boundary types (`RenderPipelineSignature/Key`, `DrawContext/Callback/Submission`, `ColorAttachmentOps`, depth policy, `SurfaceRenderPass`); header-only, no matching `.cpp`

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


`renderer.optical_geometry` (`App.Renderer.OpticalGeometry.cppm/.cpp`) now owns
bounded immutable opaque/vacuum numeric terrain fixtures, reusing same-ID
OpticalCell rows plus roundness, centre labels and explicit halo/exterior inputs.
Its analytic-double Bezier oracle rejects uncertain near-degenerate events.
Noise-free quadratic occupancy preserves rendered saddle connectivity, rather
than substituting physics' straight/disconnected contours or cell boxes.
`RadianceCascadePass::prepareTerrainTrace/prepareTerrainCascades` use FP32-only
Vulkan qualification with the existing fence/error owner and parity schedule.
Local conic/rounding certificates expose numeric_ambiguity instead of dropping
uncertain crossings; their cost and availability are unmeasured. Exact zero-entry
raw half-cell probe origins use a proven directional dual-plane ownership case
before interval subtraction; other origins retain enclosure-based classification.
Terrain uses scalar dual-cell DDA, without reusing box-vacuum flags. Extra GPU buffers cost
8 bytes/sample + 1216 bytes for shared math tables; actual candidate/previous bytes
and resource ceilings enter admission. Three PRECISE linked components remain
owned for exact SPIR-V inspection via explicit qualification-only output.
Initialization retains their shader program objects without creating terrain
pipelines. Admitted terrain trace preparation creates only the trace pipeline;
terrain cascade preparation creates only top/merge. Successful pipelines remain
pass-owned for reuse, including partial family readiness after a later failure.
The readiness query does not compile; explicit IR output may generate all three
components when requested. Existing cell pipelines retain their initialization.
The 4096-sample cap is not a world-size admission. The physical-ray trace and
ambiguity leaves passed on the preceding frozen source; the old metre-based RC
leaf still failed at row 28/status 32 with 12 local connector spans. That is an
availability failure, not a timeout or evidence of slow GPU work. Generic FTZ
and shared terrain-source admission passed ordinary/Core+GPUAV gates separately.

Terrain now uses one interval-origin/direction parametric traversal. Public
OpticalRay queries retain their physical-metre wrapper. Internal RC rays mean
p+lambda*d_raw, without normalization; connectors mean S+t*(E-S), t in [0,1].
Their endpoints remain probe+nearCells*d_raw expressions, using local half-grid
anchor differences and algebraic common-term cancellation. Singleton zero
component differences prove identical endpoints; a collapsed computation never
selects identity. Point/raw/interval bit guards precede those identities, and
uncertain sign, ownership or event order remains numeric_ambiguity. No receiver
surface contact is ignored in this phase. The independent double Bezier oracle
exposes parametric and connector reference entries, while retaining metre rays.

The existing estimator covers the complete top padded-probe rectangle: for each
axis it bounds the distance from any probe in [.5,p_max] to either world edge by
max(p_max,extent-.5); its integer L1 sum bounds Euclidean distance. Terrain uploads
require max(abs(dx),abs(dy))>=.5, hence raw length>=.5. Twice the estimator's
terminal distance is therefore a conservative terminal lambda bound. Its finite,
non-underestimating FP32 conversion is checked before allocation. The estimator,
angular/parent closure, buffer ABI/bytes and generic-cell schedule are unchanged.
Opaque/vacuum transport needs no medium-length integral; future absorbing terrain
must use deltaLambda*length(d_raw)*cellMetres (or deltaT*length(E-S)*cellMetres),
never infer metre length from lambda alone.

This parametric refinement is awaiting execution/review and does not claim the
row 28 failure is repaired. The unchanged RC assertions, metric/translation and
padded-guard fixtures qualify the declared new schedule; finite angular physical
holdouts remain required. Direct symbolic GPU endpoint-identity coverage waits
for the actual receiver query seam, rather than adding an otherwise unused ABI.
Source selection now retains the complete earliest hit-event and coordinate
enclosures, rather than choosing a near/root or coordinate midpoint. The
intermediate qualifier accepts a bit-identical common RGB source across possible
labels, the declared point-label rule at a singleton position, and certified
strictly owned two-label straight partitions. The latter conservatively encloses
the existing warp and triangle-containment arithmetic; it does not substitute a
different SDF or ask ownership across the later opaque span. Unresolved differing
sources return numeric_ambiguity. General curved differing-source certification
is still REQUIRED before ordinary-world/emitter integration, not an omitted
product requirement or a claim that all multi-material inputs are qualified.

Exact anchor subtraction uses the existing half-grid case and same-sign normal
[Sterbenz operands](https://people.eecs.berkeley.edu/~jrs/papers/robustr.pdf)
within a factor of two. A validated exact reverse subtraction a-r=b (or the
symmetric b-(-r)=a) also proves r=a-b. Candidate and reverse nonzero differences
that collapse to zero/subnormal are rejected before equality; failed witnesses
retain conservative interval fallback. No input snapping or special material
roundness grid is introduced. These operations require the existing PRECISE /
NoContraction program and no RelaxedPrecision; Vulkan's correctly rounded
[FP32 add/subtract contract](https://docs.vulkan.org/spec/latest/appendices/spirvenv.html)
is the arithmetic premise. CPU reference source/exterior hit distances also
reject nonfinite results and positive-to-zero collapse before commit.

Packed terrain, boundary witnesses/receivers, absorbing interfaces, matter
reconstruction, world activation and finite angular tuning remain separate.

## Opt-in opaque spatial integration

`RetroSettings::m_spatial_enabled` is default off and travels on the existing
owned `RenderFrameRequest::m_retro`. Editor preflights positive even output,
exact 2x raster/output dimensions, finite EV and incompatible legacy temporal or
pixelization before applying a spatial request. Palette and HDR stay independent.
The original `RetroPass::beginFrame` overload forwards equal extents; the new
explicit raster/output overload rejects spatial use without its exact mapping.
Unsupported backend/sample-rate capability is visible only for the new opt-in.
No game setting enables it automatically.

RetroPass lazily loads the existing 32-byte-ABI spatial compute program using its
injected shader service. Each of three completion-governed slots owns unresolved
RGBA16F4x color/D32 depth plus a distinct native-output RGBA16F UAV/SRV. The exact
committed descriptors enter allocation queries before image creation. Replacement
textures/views/roots commit together; accepted-submit fences cover source,
output and bindings through presentation, including failed present. Unknown
submission remains renderer quarantine. The output is sampled 1:1 before existing
EV/AgX, optional palette and the single attachment encoding; UI stays subsequent
and native. Spatial uses neither jitter nor temporal history. Legacy paths keep
their previous behavior. Allocation failures propagate; no new idle wait, queue,
resource service, hot-reload protocol or frame deadline is introduced.

`GridPass::update` takes an explicit default-false spatial policy from that owned
request. Terrain cache identity includes the policy as well as the attachment
signature, because legacy temporal also uses MSAA4. New fragment entries use
sample index/interpolation and the existing contour topology/material scores to
select point occupancy and shade each covered sample's material, normal and
emission. Geometric coverage is binary and applied once; alpha-to-coverage stays
off. Direct sample-frequency background composition reuses the existing layered
shader and a separate fenced cache, avoiding the legacy single-sample backdrop.
Its roots participate in existing submission/world retirement accounting.

The first admitted content domain is an opaque-only visual inventory (including
unused rows) and no particles. Optical-blend/refraction inventory and CPU/GPU
particle payloads reject explicitly at current-frame/preparation boundaries.
Content publication remains independent of the previous frame's sampling policy;
publication does not admit that content for spatial rendering.
Matter/translucent support remains required later. No optical
transport or contour proxy envelope changes are included. Actual GridPass sample
readbacks and real RetroPass/reference, resize, policy, background and grading
fixtures are authored in the asset suite; source and execution qualification are
pending. Prior standalone-kernel passes do not establish this integration or
ordinary-world admission, and no 4K/60 performance claim is made.

Canonical rendered roundness now means binary32 RTE of the exact two-endpoint
mean, independently of device rounding mode. `canonicalTerrainRoundnessMean`
validates finite nonnegative [0,1] endpoints, retains their authored float bits,
normalizes signed-zero results and constructs the coefficient with uint-only
rounding. OpticalGeometry promotes that declared coefficient to double for its
independent Bezier geometry/source queries; it no longer models a different
unrounded double mean for mixed materials. Subnormal encoding remains distinct
from normal-domain GPU transport admission. The shared Slang construction lives
in grid_contour_math and serves the actual raster/terrain consumers; the
isolated active-path coefficient probe checks 96/8/8/16-byte input/output strides
and the 16-byte count constant. A dedicated GPU-locked ordinary/Core+GPUAV shard
owns the new leaf, without compiling unrelated radiance pipelines just to test
this coefficient. Existing physical ray queries, source ownership semantics,
intersection certificates and numeric tolerances are unchanged.


The bounded real-scene receiver bridge is authored and awaits execution. An owned
`TerrainReceiverSet` retains the existing immutable optical snapshot, stable
midpoint feature keys, rendered-solid components and conservative per-cell
candidate lists. The shared 32-byte feature protocol keeps rounded visual centres
separate from physical symbolic contact. The independent double Bezier oracle
factors only the witnessed midpoint contact; GPU transport likewise uses
algebraic c=0 only for that feature and still certifies the full first positive
occupied region and every later hit. C1 reconstruction composes each connector
before its spatial/angular weight. Receiver-only source admission uses the
selected base 8/doubling positive-normal floor; this is an interim numeric domain,
not a production media discarded-radiance policy.

`RadianceCascadePass::prepareTerrainReceivers` computes through C1 and writes
16-byte mean-RGB/status rows without a full C0 directional output. Publication
requires completion and all receiver statuses valid; C1/direction buffers move
into a strongly owned immutable generation. Admission counts actual parity
capacities, maps/output/readback, conservative upload staging, previous working
resources and every externally retained generation. The same pass must track its
live leases; callers account resources owned by other producers in their reserve.

A complete bounded `GridStagedFrame` optical attachment is compared against the
same uploaded cell words, selected optics/roundness and metric. Vacuum boundary 0
is admitted only with that checked ringed attachment; ordinary legacy admission
is unchanged. Geometry owner/revision publishes at the resident slot/table commit,
never from consumption acknowledgment or mutable pending cell-cache contents.
GridPass rejects mismatched receiver leases and retains attempted binding leases
through partial failures, complete rebinding and completion-gated root retirement.
The first actual scene uses front-facing diffuse `albedo*meanL + S`. Its visual
continuation uses the declared FP32 squared-distance score with stable feature-key
ties within a rendered solid component, scanning only conservative cell lists.
Rounded visual centres/score never enter contact, transport, bounce or gameplay.
Normal/specular RC response, media, world scheduling and full-world budgets remain
required later; the authored bounded bridge is not evidence of their acceptance.

## Experimental visual receiver continuation

`GridPass::update` owns a default-nearest `ReceiverContinuation` policy. The
explicit `interpolated_experiment` value changes only the existing32-byte receiver
constant's enable value from1 to2;0 stays off. It is not inferred from MSAA or
transport state, and invalid enum values reject before owner-state mutation.
Existing draw-slot roots and immutable generation leases retain their fences.

The experiment uses two same-component candidate traversals and the fixed
half-cell compact kernel: `m=min(s)`, `R=sqrt(m)+.5`, positive weights
`(m/s)^2*((R²-s)/(R²-m))²` for `s<R²`, normalized. Exact receiver centres retain
their original mean; coincident conflicts keep the lowest feature key without a
universal continuity claim. The unchanged nearest receiver anchors the boundary
side: blend candidates must also have positive outward-direction dot against
that anchor. Opposite wall faces are excluded; a medial-line discontinuity is
intentional when incident sides differ. Only incident mean is mixed; source S, geometry,
material/opacity and transport inputs remain unchanged. CPU candidate pruning
encloses the entire possible support using the existing outward FP32 score bounds,
Vulkan inverse-sqrt/division error coverage and outward radius arithmetic. It keeps
stable complete lists without truncation, including sites that cannot win nearest.

The32-byte feature reuses its historical reserved u32 words at offsets24/28 for
normalized visual-outward XY binary32 bits. Capture derives the midpoint tangent
as Bezier B-A, rotates to the table's exterior, and reverses complemented connected
saddles. Finite/nonzero and equal-magnitude diagonal guards admit only the actual
axis/diagonal midpoint domain. Thus positive normal-dot sign is exactly the bounded
integer dot of component signs; signed zeros are zero, and perpendicular faces
cannot acquire a floating contraction residual. These directions never enter
contact proofs, transport, bounce or gameplay. Stride, bindings and allocation
bytes are unchanged; existing scalar reflection/offset checks remain applicable.

The app GPU kernel leaf uses the actual Grid lookup before albedo/post and an
independent all-feature double kernel. A separate physical-quality leaf fixes
blocked-side masks/bright controls for a one-cell partition and a1x1 portal with
independent CPU Bezier witness-ray boundary means. Mean leakage above1% is a real
test failure, never an expected-fail or green implementation claim. The earlier
unrestricted blend failed with16.15% mean leakage against nearest0; the authored
facing restriction awaits the same physical masks/oracle/threshold. Default nearest
stays selected until this experiment qualifies. The original GAME1x8 aperture
remains a separate visual control. No timing or full-world admission is claimed.

## Opt-in mapped diffuse receiver response

`prepareTerrainReceivers` appends a typed `TerrainReceiverOutput`, defaulting to
`mean_only`. `angular16` retains sixteen spatially composed RGB/status records
(16 bytes each,256 bytes/receiver), separately from the original16-byte mean.
The same sixteen C1 directions and four spatial connectors give64 queries;
connector-before-weight composition, old mean accumulation order and query policy
are unchanged. Angular accumulation/validation is gated off for mean-only callers.
The32-byte producer constant uses one former reserved word as its output flag.
The unchanged direction buffer is retained with its C1 offset/count in the owned
immutable generation; no new transport rays, dispatches or publication cohort exist.

Admission includes angular GPU storage, appended readback, simultaneous CPU
mean/angular validation snapshots and a scalar scratch allowance, plus previous
working state and every live generation. Buffer ceilings include the full combined
readback and angular resource. Completion validates every bin's shape, status and
finite nonnegative RGB before publication. A narrow span validator is shared by
that real path and CPU malformed-row tests; this predicate test is distinct from
an actual corrupted-GPU-output publication failure. Existing partial preparation,
submission quarantine and completion-gated strong leases cover the new buffer.

Grid's typed `ReceiverResponse` defaults to `flat`; explicit `mapped_diffuse`
requires a completed compatible angular generation. Its receiver constant grows
from32 to48 bytes with response/direction-offset/count/reserved, reflected on the
actual module. New angular/direction SRVs use the established unused dummy binding
for flat/off modes. Normal maps are read only for legacy lighting or requested
mapped response, through the same owned textures, patch UVs/gradients, scale,
positive-Z validation and front-normal fallback. Directions stay in world XY.
One candidate traversal accumulates all16 bins using the same nearest anchor,
component, facing filter and positive continuation weights. Exact front normals
use the old mean path bit-for-bit; self-emission S is separate.

The actual shader coefficient is the normalized uniform-elevation cosine lift:
`K=2*integral_0^1 max(A*sqrt(1-u²)+B*u,0)du`, with `A=dot(n.xy,d)`,
`B=n.z>=0`. Its positive-A branch is `B+pi/2*A`; negative A uses
`B+A*atan2(B,-A)`. The small `x=B/(-A)<=.25` branch evaluates the positive
factor series through x^13, with omitted magnitude bounded by `|A|*x^15/15`
before rounding; B=0 is explicit. Shading averages `K_i*L_i` over16 bins with
no extra1/pi. This is diffuse response only; rough specular remains required.

New GPU helper/lookup tests compare against independent CPU elevation quadrature
(1e-5 coefficient target), the16-direction constant-field identity (0.01 target),
weighted RGB, exact flat/front bits and facing isolation. The existing real
receiver leaf also observes opt-in bins, unchanged mean bits, C1 oracle comparison,
mapped admission and retained leases. Its additional dispatch is a second test
batch, not extra queries in one producer solve. Execution, actual mapped game
images and cost remain pending; CPU calibration alone is not GPU qualification.

## Exact-view cavern backwall lighting (authored, execution pending)

`prepareTerrainReceivers` optionally requests `TerrainBackgroundOutput::c0_mean`.
It executes the existing C0 merge and reduces8 directions to16-byte RGB/status
rows. Default mean/angular foreground outputs preserve their old evaluation.
The same completed generation retains the immutable query inputs by transfer;
old root bindings are detached, and later preparation allocates a new cohort.

`prepareTerrainBackgroundView` / `encodeTerrainBackgroundView` /
`acquireTerrainBackgroundView` produce one owned exact-camera view: centre plus
four admitted Vulkan standard indexedMSAA4 positions per raster pixel. Four
short single-dual-cell connectors compose with C0 means before weighting.
The receiving plane is exactly [.5,width-.5]x[.5,height-.5] in local fine cells;
its outer half-cell band is declared far void, with zero radiance. No parent
clamping, omitted blocked support or weight renormalization is used. Parallax
changes decorative albedoUV only; lighting stays anchored to physical worldXY.

GPU counters count EVERY expected5-position sample, including far void, plus
attempted/qualified/unresolved connectors, arithmetic-bound failures and invalid
values. Acquisition checks exact expected sample/query shape, bounded counts,
zero unresolved/invalid results and eligible-refinement completion after the
accepted fence. Only32 aggregate bytes (plus existing16 refinement bytes when
selected) are read back for view publication. No full-view interactive readback.
The optional output is80 bytes/raster pixel (~158.2MiB at1920x1080), plus the
source field/query resources and counters. Checked admission counts working,
CPU validation, externally leased source/view generations and largest resources.
This is bounded<=4096-cell support, not full-world or frame-budget acceptance.

Grid's default `BackgroundResponse::unlit` is preserved. Explicit `lit_backwall`
requires a matching validated view/source/current resident geometry and exact
camera/extents/device. Cached1x reads centre; directMSAA reads its actual index.
Both multiply decoded linear texture albedo by incident radiance before exposure
and AgX. Missing/mismatched lighting fails; completed roots retain strong leases.
Every cached backdrop preparation binds/stamps its source-view identity.

The GAME capture adds matched stone-albedo lit/unlit controls for closed/open
rooms and the environment scene, retaining sourceS, camera, EV and flat foreground
response. Separate timestamps cover worldRC, exact-view reconstruction and
background raster. Source review/build/GPUAV, physical holdouts and images remain
pending; no performance or complete product-GI claim follows from authored code.

The bounded lit-view API admits affine orthographic cameras with exactW=1,
axis-aligned +Z rays and a receiving plane between near/far. Ordinary pan/zoom
preserve this domain. Parallel/near-parallel tilted, backward and perspective
views reject before GPU allocation. Every GPU sample additionally checks finite
homogeneous divisions and a forward plane intersection; legacy unlit fallback
mapping is not used to manufacture a lit query. Re-acquiring a published view
returns the same immutable lease after fence/state checks without readback or
statistics mutation. Generic trace/read entrypoints reject background batches.

## Completed-picture retention for experimental interactive lighting

Retro's private scene render pass initializes the supplied context viewport and
scissor before invoking scene content, matching Renderer submissions. Grid's GPU
indirect draw also establishes its own one-viewport/one-scissor state, so terrain
without a background never depends on a prior draw to initialize dynamic state.

RetroPass optionally protects one completed SceneTarget picture within its
existing slot ring. Accepted content writes promote only after their fence;
accepted readers extend the slot's last-use fence. Repeated retained draws wait
through the existing fence hook before mutating the slot-owned shader root.
Preparation excludes the protected slot and restores prior prepared state on
failure. No second image cache, queue or temporal solver is introduced; retention
rejects temporal history mode. Default callers preserve their original policy.

Editor owns the bounded receiver producer and exact-view resources on the existing
render lane. Candidate field/view completion precedes actual Grid activation.
Camera changes prepare a new immutable view while keeping the last completed
picture; source/view leases survive accepted draws and are released only after
quiescence or device-removal proof. A failed unsubmitted view/frame keeps that
picture; unknown completion retains the established fail-stop policy. The
experimental caller requests mapped diffuse foreground and lit backwall without
changing transport mathematics. Physical quality and interactive timing remain
unqualified, and no full-world scheduling is implied.

Capacity rejection retries only a still-prepared lighting batch whose Renderer
fate is rejected, whose preparation callback never ran and whose accepted fence
did not advance. Numerical failures are not retried as capacity pressure. Stale
world cancellation first checks the actual Grid candidate triple, then the exact
loading lighting cohort; active camera refreshes remain owned and fenced. Only
known lighting-domain/admission failures at their preparation/readback origin may
retain the picture; generic Renderer/UI/device/quarantine failures propagate.
The retained slot also owns its request timestamp for honest displayed-view age.

Across resize, a retained spatial picture keeps its original native viewport at
(0,0). The scissor intersects that viewport with the real target: larger surfaces
keep the cleared letterbox, smaller surfaces clip. The fragment shader addresses
from interpolated UV/source extent, so this requires no shader change or light
resampling. UI remains on the current target. Actual presented metadata carries
both the old picture viewport and current target extent for clipped picking.

## Native-cell world radiance candidate

`engine.app:renderer.world_radiance` (`App.Renderer.WorldRadiance.cppm/.cpp`)
owns the opt-in v22-informed experiment independently of the legacy exact-contour
control. `WorldRadianceSource` copies native material IDs and selected vacuum,
opaque or homogeneous absorbing/emitting optics with the true metre origin/metric.
Unavailable used models reject; scattering and particle optical sources are not
implemented. Its immutable true-vacuum hierarchy and packed two-u16 ID words use
40-byte sigma/j/S palette rows. `WorldRadiancePass` uses the existing render-owner
encoder/fence and two actual FP32 interval parity capacities, then64 actual-direction
footprint connectors per spacing2 refined probe produce signed order4 CH. The
112-byte row keeps DC and eight RGB AC terms plus status; finite negative normal
convolution is retained diagnostically, with only final physical diffuse response
clamped nonnegative and self-emission separate. Front normal is exact DC.

Aggregate counters reduce per workgroup. Fence-completed publication requires all
expected rows, complete query counts and no invalid/status failures; repeated acquire
returns the immutable generation before mapping/mutation. Buffer/source leases and
previous candidate overlap are budgeted before allocation. Unknown submissions retain
resources pending explicit quiescence/removal proof. The source edge is bounded
at4096 cells; this is not full-world resource/performance qualification. Dense
receiver rectangles may cover only requested pages without cropping the source.
Legacy exact diagnostic APIs are unchanged; fine-contour image qualification is open.

Measured direct-window preparation performs a CPU-only first-bounce preflight
for an initial/changed immutable source, skipping unchanged-source window refresh.
It counts actual native opaque faces adjacent to vacuum, homogeneous medium or
the declared exterior into 32x32-page/axis counts. Existing emitter bounds and
256 directions conservatively count planned outward rays, with a one-cell support
halo for face/empty-side centres. These are page-mask upper counts, not exact
visibility or per-face tracing. No face GPU table/kernel or material/DDA change
is implemented by this observation.

The same record charges proposed RGB32F face values, conservative four-u32/cell
lookup, CPU upload staging and old/new bundle overlap; it derives requested-window
C1-C3 recursive support/parity/connector bounds with a full-source terminal exit.
Current/retained producer owners come from the existing admission ledger. External
reserve is explicitly an estimate, not measured whole-device live memory. Future
resource/peak excess reports NO-GO without rejecting or changing the current
direct-light baseline. Counts do not establish GPU cost, 4ms/1s acceptance or a
complete first-bounce implementation; the 512MiB resource guard remains unchanged.

Phase A adds a distinct `WorldRadianceDiscoverySettings`/batch/immutable result
to that SAME pass. It requires the completed matching normal generation as an
owning same-device donor for immutable packed-ID/material/hierarchy GPU inputs.
One additional discovery program is compiled lazily after the first admitted
discovery demand and reused thereafter; direct-only initialization creates only
the original solve/refine pipelines. It uses the same local parent-query
geometry helpers and native DDA as solve/refine; no radiance-value pruning or
CPU trace pipeline is introduced. The full-source terminal closure is retained.
There are no face moments, albedo projection, indirect-field publication or
Phase-B lookup allocations in this core checkpoint.

The discovery bitset uses four bits per native cell (8MiB at4096²); a bounded
u32 key list uses `min(4*cells, checked trace upper bound)` capacity. Native face
keys encode cell index and side, with the agreed exposed-face/concave-axial-owner
corner convention. Atomics deduplicate actual first entries. Overflow/invalid
geometry fails completion; a strict opaque start or escape has no incoming face.
The result owns fresh bitset/list buffers and source/cohort/window/request epoch,
so a later result cannot overwrite retained earlier data. Current/retained owner,
root, reset, readback and estimated external bytes are admitted before allocation.
Fixture key readback additionally charges a worst-capacity CPU key vector in peak
storage; non-readback runtime accounting is unchanged.

Each encode admits at most262144 trace-call upper bound and a strictly newer
actual frame token, including across canceled/replaced discovery batches. Advance
only settles fence/query/readback state, never encodes. Every quantum must
complete EXACTLY its fixed positive-dependency trace count; a smaller count is
incomplete, not a successful cull. First-quantum timing
includes bitmap/counter reset. Existing submit/discard/unknown quarantine and
positive-fence cancellation govern the separate batch. The shared app GPU leaf
checks exact entered cell/side and direct L/T, including simultaneous native
transverse crossing at a skipped vacuum-block exit, exact scheduled trace totals,
offscreen keys, dedup, overflow,
same-frame rejection, canceled-fence proof, source identity and old-result data.
The distinct result also reports actual incoming-face hit occurrences and
conservative offscreen distinct faces (owners outside the requested rectangle).
These use four u32 state words plus the existing eight statistics words, a48B
readback, with all storage charged. Per-quantum occurrences remain u32-bounded;
host totals are u64. A/B core windows assert outside/inside counts explicitly.

Editor/Game now schedule the observation only on direct-window +measurement
+explicit capture. The existing actual-frame submission/fence boundary supplies
one quantum after complete direct input; polling never submits. Pending work
retains the completed direct field/picture. Source/cohort/exact query-window
changes cancel/drain before replacement; camera revision churn alone does not.
Old immutable discovery buffers survive B preparation under their own lease and
charged overlap. A real Game-owned +32-native-X pan separates fresh A/B receipts;
no Renderer camera or public frame payload was added. Actual4096 work/budgets and
camera acceptance remain UNRUN; moments/indirect publication remain held.

`App.WorldRadiance.Tests.cpp` registers the separate `app/world_radiance_gpu`
group in the existing app test root. One shared RHI/producer fixture checks native
cell-slab ray references, constant/DC/K16 normal invariants, the same12 closed/open
aperture points and source S, and post-producer generation leases. Open brightness
retains15% L1 and closed leakage retains1%; the measured failures and next bounded
control are recorded below. `PPR_TEST_WORLD_RADIANCE_CANDIDATE=ON` adds ordinary and Core/GPUAV CTest
entries; every older shard explicitly skips the experimental group even when OFF.

The first actual spacing2 native candidate failed open-point brightness by92.33%
(relative L1), with zero closed-side leakage; constant/normal invariants passed.
It is unqualified and Grid wiring remains held. The next bounded control exposes
only refined spacing1/2 (default2), stores that spacing in immutable generation
metadata and decodes field positions from it. Native1 centres are i+.5, so the
SAME12 physical points are exact output probes. Coarse spacing,4-cell C1 footprint,
phase, interval operator, source S and masks remain unchanged. Only if full native1
fails, the SAME shared fixture runs cascade_count2 with C1 terminal escape and
prints its independent metrics before asserting the FULL candidate gates. This
terminal control changes terminal angular quadrature and merging; it is not a pure
merge proof and cannot qualify a failed full candidate. No spacing2 failure rerun,
phase sweep or full-world storage admission is implied. At4096², two112-byte native1
CH generations plus the two FP32 parity capacities already exceed the provisional
4GiB lighting budget; compaction/storage and GPU timing remain separate unmet work.


The native1 full cascade subsequently failed56.87% open L1; its C1-terminal
control failed69.42%, both with zero closed leakage. Those measured controls
remain rejected; no footprint/phase tuning or Grid wiring follows from them.
The next explicit `WorldRadianceTransport::straight256_reference` mode is a
small-scene native-cell-centre reference, not a replacement scalable RC solver.
Default `cascade_diagnostic` remains unchanged. Straight mode requires spacing1
and phase.5, stores its transport choice in the immutable generation, and writes
exactly width*height signed112-byte CH rows at i+.5 (minimum index0). It uses the
existing refinement pipeline's uniform mode branch, existing hierarchy/DDA and
first-opaque/actual-world-escape boundary. It uploads256 fixed midpoint directions
and one terminal descriptor, skips every coarse dispatch/root/atlas, and accounts
one32-byte unused parent binding required by reflection. At64x36, exactly589824
straight queries are admitted; publication requires that exact completed count.
The CH projection uses actual uploaded directions and the same DC-centering/window,
with exact common-input DC and zero AC. No camera/view resource is created.

The existing single shared GPU fixture retains constant/DC/normal/admission/lease
checks, then evaluates all12 fixed points in each unchanged door state. It reports
1024-direction physical references, independent slab references using the same256
uploaded binary32 directions/source values, and GPU DC separately. Sampling error,
GPU traversal agreement (abs1e-5+rel1e-4), open physical15% L1 and closed1% leakage
are distinct outputs and assertions. Both native spacing failures remain archived;
passing this bounded straight reference would not establish scalable RC, fine
contour quality, full-world storage or GPU timing. Foreground incident-side
reconstruction and Grid/Editor wiring remain unimplemented for this mode.


## Small-scene straight reference showcase bridge

The straight256 producer passed its fixed native-cell physical holdout at5.785%
open brightness L1 and zero blocked leakage, including the actual Core/GPUAV run;
matching uploaded256 CPU slab/GPU values agreed at all24 points. This qualifies
only that bounded reference. Cascade spacing2/native1/C1-terminal failures remain
rejected; scalable RC, fine contour images and the4GiB/4ms/1s/4K60 goals remain open.

The explicit interactive showcase now requests the same typed straight256 source.
Editor owns `WorldRadiancePass` on the existing render lane, initializes native
pipelines while loading, submits through the existing fenced1x1 attachment route,
and admits the Grid candidate only after immutable field publication. Current
pan/zoom reuse this world generation and change draw constants only. There is no
`prepareTerrainBackgroundView` call or per-camera160MiB exact-view buffer in this
path. Accepted/unknown submit settlement, cancellation cohorts, retained completed
pictures, actual displayed-camera picking and inverse teardown keep their existing
contracts. A producer GPU timing is logged only if its completed submission fence
matches; it includes the tiny1x1 attachment work, not CPU compile time.

Grid's optional native attachment is mutually exclusive with the legacy exact
geometry. A complete reset/full-refresh compares all actual uploaded IDs, every
selected opaque source product, metric, crop and all outside-crop uploaded vacuum
before staging. The CPU owner/revision follows the actual atlas mapping and
resident-table publication. Unattached policy changes reject; incomplete/replaced
uploads invalidate future compatibility. The completed generation setter checks
job/world/content/revision, source identity, native spacing/min/count,112-byte stride
and actual buffer size. Per-root slot leases and backdrop-slot identity retain
old resources through accepted readers, partial binding failure and retirement.

`world_radiance_types.slang` extracts the existing signed CH/K16 consumer unchanged.
`grid_world_radiance.slang` provides the shared48-byte Grid constant and full112-byte
field binding; both tile/background shader reflection checks the real layouts.
The existing shader-directory staging includes these headers without a new module
or separate staging system. The producer constant grows48->64 bytes to carry maximum
selected S. Direct publication first checks each coefficient against FLT_MAX/256
and S against FLT_MAX/8, then finite8*sum(abs(CH))+2*S. Since K16<3 and reflectance<=1,
this conservative domain leaves substantial arithmetic headroom and rejects
unsupported extreme brightness visibly, including saturating-overflow devices.
No clamp/source alteration or legacy cascade-domain change is introduced.

Foreground chooses the nearest exposed native face among the four Grid label cells
matching the drawn material, with FP32 squared distance and stable(cell,face) ties.
Only an in-crop actual vacuum neighbor supplies CH; the16-neighbor local search
never reads opaque self-S as incident light. No qualified face means zero incident
plus its own S once, a legitimate deep-opaque result. Mapped normal decoding and
world-direction K16 are reused, with final nonnegative convolution only. This is
a native-face rim approximation, not rounded-contact recovery or the later
screen-space fine-detail pass. Legacy unlit callers retain their previous branch.

The backwall uses the same directional field and a front normal at actual worldXY.
Its receiving plane is the closed native crop[0,width]x[0,height]; outside is
transparent far void. Only the interior outer half-cell strip clamps to an edge
centre; nothing beyond the crop receives light. Explicit affine orthographic +Z,
finite forward-plane cameras are admitted; the existing checked receiving-point
shader helper is used. Texture parallax affects UV only. Cached and directMSAA
backgrounds bind the same source generation, and cache identity includes that
lease. Fine occlusion-preserving reconstruction is still an unqualified image gate.

Focused coverage stays in the existing shared world fixture: the actual Grid
consumer entry checks two exposed rims, deep-opaque zero, mapped K16 and all four
crop edges/inside strips/outside; a finite-brightness failure rejects publication.
A CPU Grid leaf checks same-ID source projection, outside-crop data, palette,
revision/reset and unattached policy rejection. The real game CPU showcase stage
now expects the native attachment and retains catalog/actor/spatial predicates.
Affected ordinary/CoreGPUAV fixture runs and ready 4K reference captures passed.
Fine-image, scalable-cascade and product performance qualification remain open.


The straight256 publication settings can request a finite positive maximum
material response. The default retains the FP32 control range; narrower ceilings
are supported only by that reference mode. The existing64B producer constants
reuse their last scalar at offset60 as a float ceiling. Existing direct-row
finite checks also reject8H+2S above it before immutable publication. Grid native
binding requires a qualified ceiling no greater than65504, and the Editor requests
that ceiling for its RGBA16F scene target; an FP32-only generation cannot bind.
No source/coefficient clamping or transport change is involved. Layout/count
readback and existing aggregate status remain the publication proof.


The initial native-reference presentation experiment retains physical backwall
albedo×incident and adds only a weak neutral mapped-normal cut-face term in the
foreground shader. The term is local render work, not transport, feedback or
source emission. The showcase owns separate mid-value stone background assets;
frontier/hollows and old headless scene bindings remain unchanged. Matched staged
shader literal0/0.04 captures keep camera, EV3, field/source inputs and assets
fixed. This does not qualify the unresolved fine-image/scalable/performance goal.

## Optional world-field GPU observations

`WorldRadianceSettings::m_measure_gpu` defaults off. An enabled batch owns a
two-query timestamp pool, written in its original encoder and covered by its
actual submission fence. Scope includes the producer's encoded transitions,
clear/compute work and statistics copy, plus explicitly requested coefficient
copy; CPU preparation/allocation/source uploads and the caller's render work are
excluded. No query allocation or writes occur while disabled.

`WorldRadiancePass::pollTiming(cohort, completed)` returns a CPU value with the
actual cohort/fence, typed status, optional milliseconds and error. It never
waits or submits. Both application-fence completion and backend host readiness
are checked; pending, unavailable/error and retired-unresolved observations are
explicit. The existing batch owns the pool through cancellation and unknown
submission/quiescence. Published coefficient generations remain immutable.
Query-pool native storage overhead remains opaque; it is not measured VRAM.

Editor retains stable source-request/publication stamps separately from camera
view age, using the sampled Application timer. The opt-in showcase metrics path
reports these alongside the producer observation. An unchanged static source's
age is not evidence of normal-play edit latency.

## Opt-in final surface readback

`SurfaceSettings::m_readback_enabled` is a constructor-only capability, default off; Renderer preserves it across resize/vsync surface transactions. An optional owning CPU output on the snapshot `renderAndPresent` overload records one texture-to-buffer copy after all surface passes in the same encoder. The fixed Renderer staging slot distinguishes unsubmitted, unknown-submit and accepted-fence ownership; unknown teardown retains it until idle/removal proof. Presentation settles before the actual fence wait/map/copy/unmap can publish `SurfaceFramePixels`. Only RGBA8/BGRA8 UNORM and sRGB display formats are admitted. Off requests allocate/copy/wait nothing. A 4K diagnostic holds about 33 MB each of GPU staging and CPU bytes; this is not a performance gate.

Readback requests use a portable copy layout with row alignment equal to the maximum of 256 bytes and the validated power-of-two device requirement, a multiple of both the backend alignment and four-byte texels. At 3840 pixels the 15360-byte row is already aligned. CPU storage retains this reported pitch, copies only each row's meaningful pixel bytes, and leaves all row padding/layout tail zero. The optional `image_acquired` output is independent of presentation/readback success and distinguishes proven pre-acquire rejection from every acquired-image fate.

Completed Retro picture slots retain the optical revision alongside existing
world/camera/extent/timestamp identity. Restoring a picture reports that stored
revision; it cannot acknowledge a newly activated field merely because world IDs
are unchanged. This metadata does not alter scene textures or reconstruction.
The existing retained-picture GPU fixture checks the protected revision as well
as fenced pixels across rejected preparation. Renderer readback ownership/fates
and its fixed staging slot are unchanged by the client-side capture sequence.

Editor field submission now retains an explicit non-idle unknown-fate stage when
no accepted fence and no definite Renderer rejection exist. Prepare, advance,
cancel-ready and current-field reuse remain blocked; only the existing positive
terminal quiescence/removal proof releases the pass owner. Entered callbacks that
were definitely unsubmitted invoke the pass's existing discard operation even
when encoding itself failed. Pre-callback capacity rejection keeps its existing
prepared retry without discarding or labeling it unknown.

## Temporal RC viewport correction

Temporal lighting selects `GridPass::BackgroundResponse::screen_contact`.
Each background ring slot owns a viewport-sized R32Uint terrain contour mask
and two 16-byte records per receiver block, capped at 32,768 blocks. The block
edge is the smallest of 2, 4 or 8 pixels that fits this cap for the admitted
viewport. At 1920×1080 this produces 240×135 blocks. A 64-lane compute group
builds pixel eligibility and neighbour connectivity and traces 16 near rays
per block. Both background paths combine the signed result with the published
world radiance. Gameplay continues to sample the published field directly.

## Bounded screen-contact backwall pilot

`GridPass::BackgroundResponse::screen_contact_pilot` is a default-off debug
comparison, not world transport or a foreground lighting replacement. It requires
a matching half-qualified native opaque/vacuum generation and a finite aligned
orthographic current view, with raster extent at most1920x1080. The existing
background ring owns one R32Uint centre-material target and a fixed32768-row,
16-byte signed-RGB/status buffer per used slot. The mask calls the actual
displaced `shadeGrid` material selector before albedo/lighting; foreground
shader output remains the ordinary path.

`prepareScreenContact_` projects native world ROI[34,38)x[7,15), intersects the
current viewport, and centres a capped128x256 raster region in that intersection.
Only actual current raster centres in that ROI and vacuum are refined. Each of16
directions samples8 taps over2nativecells, returning first opaque S or signed CH
at the near endpoint/actual screen exit; crop exterior is declared far void.
This is sampled approximate visibility. Final front-normal convolution clamps
only after signed accumulation. Both cached1x and direct4x background paths load
the same centre-density result; there is no per-sample repeat or history.

The mask is transitioned from render target to SRV, then compute output from
UAV to SRV with explicit barriers. Existing selected-slot proof precedes any
replacement/root mutation; partially encoded pilot work is fenced even if later
preparation fails. Roots and the slot retain the field, and positive teardown
proof remains required. Default-off mode creates no pilot pipelines/targets and
uses existing compatible dummy binding facilities. `GridPassObservations` counts
actual retained pilot mask/buffer bytes. At1080p three masks cost23.73MiB plus
1.5MiB incident buffers, before driver/root/overlap costs; no timing claim.

Parameters are32bytes of eight u32 scalars, results16bytes of three FP32 values
and u32 status. Reflection/offset checks own that schema. The published field's
8H+2S<=65504 admission dominates the pilot front-only convex response
<=max(H,S). Unexpected invalid rows display explicit magenta and fail pilot
visual acceptance; fixture readback inspects every active status. This debug
policy does not qualify general product error reporting or fine-image accuracy.

`ApplicationEditor` prepares resident visibility before the optional mask and
background, skips all new work for a retained completed picture, and transports
the request through owned staging data. `App.WorldRadiance.Tests.cpp` extends
the existing single-device/single-Grid native leaf with constant, closed/open,
1x/4x consumer, foreground identity, dense few-point mask-ray references, camera,
cohort and fence controls. No new application/test framework or CPU cascade.

### Sparse world-field preparation observations

The existing `m_measure_world_radiance` / `WorldRadianceSettings::m_measure_gpu`
flags additionally gate sparse logger phase records. Editor identifies candidate
staging, candidate GPU-resource upload progress, accepted producer submission,
and host-observed completed-fence acquire. Two grouped diagnostic flags in the
existing `TerrainLightingState` emit candidate upload begin once and end only
when ready, with no record on repeated pending/credit polls. Every record carries
the actual job/world/content/revision and available command sequence/fence.

`WorldRadiancePass::prepare` retains its `WorldRadiance.PrepareCandidate` CPU
scope and logs prepare, buffer/source upload, and root/timestamp-query setup
boundaries. These CPU preparation phases exclude the existing encoded-producer
GPU timestamp interval. A missing end record plus the existing propagated error
indicates an unsuccessful phase. Host-observed fence readiness includes when
Editor next polls after candidate work; it is neither physical presentation nor
a measured GPU queue-wait interval. No transport math, source policy, queue/fate
protocol, synchronization or new profiling service is introduced.

## Direct-window real-world source and page lifecycle

`planWorldRadianceReceiverWindow` admits the existing affine orthographic +Z
receiving plane, includes a two-native-cell lookup halo and every explicit
additional gameplay receiver demand, then bounds the union to aligned32-cell
pages within the complete source. Sparse indirection/eviction is absent. Large
or disjoint demand therefore pays for its entire bounding rectangle or rejects.
Grid checks visible lookup coverage before draw; missing coverage is not black,
sky or clamped into a smaller field. A defensive invalid row remains visible.

A direct-window batch allocates one private112-byte-per-row rectangle, existing
compact source/hierarchy inputs, immutable per-page roots/masks and bounded
statistics readback. `encode` submits only its current page; `advance` collects
its actual completed fence/status and host u64 work aggregates without submitting
anything. `acquire` publishes only the complete immutable rectangle. Same-source
CPU storage is counted once across live generations; independent fields and GPU
input allocations/overlap remain counted. Full4096² output alone is1.75GiB and
fails the unchanged512MiB single-resource ceiling in the explicit admission stress;
that rejection is not all-pages/full-world budget success.

Editor begins page work only from an actual nonobsolete render request, with a
frame-ID one-page limit. Control polling never loops page submissions. Camera
motion within completed coverage reuses its field; outside coverage retains the
last completed picture while a whole replacement rectangle is prepared. Exact
submit fate, unknown quarantine, cancel drain, source/cohort identity and inverse
teardown remain unchanged. Source uploads/CPU preparation are outside producer
GPU timestamps and must be reported separately from measured encoded page cost.

Native4x optical composition keeps the existing background/terrain/liquid/solid/
gas/actors/Hable order. Optical inventories also prepare the existing fenced1x
RGBA16F background cache; uncovered samples still use direct sample-frequency
background. Final optical programs/pipelines/compatible roots are selected by
spatial policy independently of sample count. Geometry-only roots bind only
materials; final readers retain the actual generation and backdrop in the
completion-selected root owner before fallible writes. Each particle slot owns
its final cache while mutable reconstruction textures remain fresh/fenced.
Native output uses ONE/(1-a); legacy/temporal programs keep straight-alpha blend.
No Renderer/Retro ownership change, omitted matter or nonspatial fallback exists.

The existing shared native app fixture retains aperture15%/1% and independent
straight256/1024 controls, adds real window/culling/distant-source/multi-page/
nonblack-sky/admission and analytic homogeneous controls, and retires the rejected
plane arm. The existing shared asset spatial leaf reads all four raw samples for
optical/matter/refraction, additive/zero/tiny-alpha emission and old-reader policy
switches. These authored checks require ordinary/Core+GPUAV execution before any
actual generated-world claim. Aggregate particle HDR safety remains unqualified.

### Direct-window overlap and bounded fresh strips

A new window may copy exact intersecting native coefficient rows only from this
pass's completed, published same-device generation with identical immutable
source owner, full cohort and transport/output settings. Memory limits and the
window itself are not sampling parameters. The destination remains a separate,
private output; its donor is strongly retained through the accepted copy fence.
A miss follows normal full tracing. No cross-source/edit/config reuse, global
cache or mutable-generation alias is introduced.

Publication tiles and emitter-direction masks stay 32x32. Direct-window settings
admit `m_receiver_rows_per_quantum` as multiples of32 in [32,1024], default256;
other transports ignore it. Fresh strips use `min(32, quota / actual_page_width)`
rows of the page, with a final remainder as needed. The same admitted quota,
capped by page size, bounds dispatch and u32 segment-counter admission. The
shader, phase, 256-bin order, output indexing and native DDA stay unchanged. The
first quantum copies the whole exact overlap row by row, then traces its strip;
a fully covered window has one copy-only quantum. Final publication requires
all copied and traced rows plus exact fresh-query accounting. Quota changes do
not disqualify an otherwise identical donor. Invalid quota rejects before batch
mutation and preserves the usable generation. CPU statistics
separate copied rows from traced work. All strip metadata/roots and concurrent
old/new fields stay in admission accounting; no new GPU ABI or shader changes.

`WORLD_DIRECT_QUANTUM` records the admitted receiver quota and joins real request
frame ID and accepted fence to the
producer timestamp, including copy, transitions, clears, trace and statistics
readback. Disabled/unavailable timing is explicit, never budget success. Source
uploads/CPU preparation and other lighting costs remain separately untimed.
The shared native fixture compares overlap+fresh and copy-only results byte for
byte against cold output, verifies source/cohort/response misses, rejected
replacement and retained-donor contents, with the existing application/device.

### Single-bounce immutable window

`WorldRadiancePass::prepareBounce/encodeBounce/advanceBounce/acquireBounce` use
a completed same-pass discovery, pristine direct donor and exact source/cohort/
window/epoch. The private batch owns compact lookup/moments, two guarded coarse
parities, indirect and combined window outputs. Its table is checked power-of-two
capacity>=2*faceCount, zero-initialized, and stores append ordinals+1 with full-key
comparison. Zero faces explicitly initialize an empty table. No global cell
lookup, CPU sort/readback construction or general cache is involved.

Each frame quantum accounts at most262144 trace calls, using the existing
positive dependency schedule:16 exact child-origin edges per C1/C2 record,
one terminal C3 trace and64 exact C1-origin connectors per refined row.
The aligned128x128 window closure requires1,701,888 discovery/replay traces each,
before outward face moments. Clears, barriers, compute and statistics copies
are inside GPU timestamps. CPU planning/uploads/first lazy compilation are
separate. New and retained output/source/key storage, staging and overlap remain
in admission with the512MiB resource ceiling and estimated external reserve.
No moment/table/indirect result is exposed before complete finite status/fence
proof. Rejection leaves the direct donor intact; cancellation/unknown fate and
positive teardown proof follow the existing pass lifecycle.

`gridFilteredAlbedoMean` is the single existing coarsest admitted image-mean
implementation used by GPU palette preparation and `gridMaterialDiffuseAlbedo`.
The latter projects linear tint*mean*(1-metallic), with white for the established
untextured solid convention; unavailable images reject. Fine textures/normals
and native consumer112-byte fields are unchanged. Shared native controls cover
zero-A identity, finite midpoint-K colour/medium bounds, sealed/around-corner/
offscreen transport, zero faces and isolated corrupt/foreign rejection. The
private missing-table-key branch is source-reviewed, not directly injected.

## Opt-in scene-linear bloom

`RetroSettings::m_bloom_enabled` appends one default-off control and requires
HDR plus current-frame spatial or temporal2x reconstruction. Validation rejects unsupported policy
before frame mutation. `RetroPass` lazily creates two compute programs from
`retro_bloom.slang`; each completed scene-ring slot transactionally owns two
RGBA16F UAV/SRV images at ceil(output/4), with separate roots and the existing
producer/reader fences. The 4x4 extraction precedes averaging, using luminance
threshold exp2(-EV) in scene units. A normalized 5x5 binomial blur and four-load
bilinear upsample add one eighth glow in FP32 before the existing single
exposure/AgX call. Scene sampling, palette and native UI remain independent.
At 3840x2160 the two images total 8,294,400 bytes per slot (24,883,200 across
three slots), excluding allocation overhead and resize overlap. Retention keeps
the matching bloom/settings and samples the old native picture UV when clipped
or letterboxed. These additions are authored; runtime/compatibility/cost
qualification must be reported from the corresponding validation evidence.

## Opt-in temporal2x and reactive scene attachments

`RetroSettings::m_temporal_2x_enabled` is append-only/default-off and exclusive
with pixelization, legacy temporal and spatial-only modes. Its exact scene
signature is `[RGBA16Float,R8Unorm]`, D32Float,4x, with positive even output at
2x raster extent. Retro owns and clears the reactive MSAA attachment, current
Hable output and a distinct output-resolution scene-linear history. A lazy
`temporal_2x.slang` compute program consumes seven actual raw samples per
corrected Hable bucket, depth and reactive support, then reconstructs history
with full nonzero Catmull-Rom support. Current reactive pixels use finite alpha
-2; background uses-1. Old reactive taps force exact current while permitting
current static metadata to recover. Moving parallax backgrounds reject history.
Bloom and present consume final history; display-only policy never resets it.

`RetroLightingPublication` copies actual bound-generation completion fence,
bounce epoch and probe min/count alongside the existing matching cohort values.
No extra lighting GPU owner is retained merely for identity. A joint ring walk
excludes history and retained picture; accepted fresh scenes alone commit
history/jitter. Retained display state cannot replace the history producer's
identity/camera. Failures preserve accepted history and do not advance phase.

Grid/Triangle final native MRT variants select this exact signature, retaining
old one-target programs and intermediate splat/cache passes. Target1 is a binary
positive geometric mask with independent MAX blending; the same selected
material/sample/mask/discard evaluation drives color and reactive coverage.
Static opaque terrain/background write0; actor triangles, optical terrain and
all final matter/disks mark actual coverage even at zero color opacity. Editor
routes native sampling for spatial OR temporal2x and supplies the actual bound
publication value. No Renderer API/type or Game activation is introduced.

At3840x2160, extra history payload is199,065,600 bytes across3slots and reactive
MSAA payload is24,883,200. Including existing raw MSAA color/depth, Hable current
and bloom gives746,496,000 bytes across3slots before resize overlap/driver
overhead. These are descriptor payloads; runtime allocation, compatibility,
image/history behavior and4K timing qualification remain pending.

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


### Immutable sparse committed-cell optical experiment

`WorldRadianceInput::m_overlay` borrows sorted unique56-byte scalar native-cell
rows only during capture; `WorldRadianceSource` owns their copy and a checked
ordinal/full-cell-key table. The96-byte source constants append row/table counts;
reflection checks the new row stride/offsets. Direct, discovery, moment and reflected
batches retain the same immutable overlay buffer leases under existing fate rules.
Hierarchy/emitter/finite-FTZ summaries include composed terrain+overlay transport.
No Game row projection or dense float cell grid is added by this analytic stage.

Discovery key capacity is bounded by the minimum of4*sourceCells,
4*(terrainOpaqueCells+overlayCoverageCells), and traceCalls+4*strictPartialCells;
zero uses one inert binding. Each quantum's calls*maxNativeSegments fitsu32, with
fenced occurrence totals accumulated in checked hostu64. Device rows/table/header,
CPU-owned copies and retained source overlap are included in existing admission.
The same shared native GPU leaf adds exact partial/overfill/boundary/lookup controls
and preserves existing full-opaque brightness/leakage and lifetime gates. No
world-scale optical quality, budgets, or dynamic matter solver integration is claimed.


### Pass-owned committed-Matter projection core

The camera-free world/particle input identity,32-byte metadata and committed view
now live in `renderer.types`; the Grid metadata factory remains Grid-owned.
Metadata word28 carries Game's authoritative physical fill before gas display
radius conversion. Legacy zero remains presentation-only/unavailable for projection.

`WorldRadiancePass::prepareMatter/encodeMatter/advanceMatter/acquireMatter/cancelMatter`
reuse the existing owner queue, submission fate and shutdown proof. The first
accepted snapshot freezes all sourceRow/gas rows before culling; subsequent stable
radix/reduction phases read only pass-owned records. Readback is a40-word summary,
not particle positions or optical rows. Accepted outputs retain one canonical
static base and their own rows/lookup/ancestor bitset. `bindMatterIdentity` is a
small immutable real-terrain-identity binding, not a second lifetime authority or
copy of world arrays. Missing identity cannot enable projection.

Accepted matter statistics also record the actual per-channel composed per-cell
emission maxima on participating (non-opaque-base) cells and their cell-count
bound from `admitWorldRadianceEmission`. Negative or
nonfinite maxima and unrepresentable bounds reject the quantum without publishing;
no row is clamped or omitted. The receipt's recorded aggregate bound is also
checked against the live display exposure at receipt construction; this is
measured-scene aggregate admission, not whole-scene HDR safety.

Only the last acquired source is the changed-cell baseline; its compared identity
is explicit. Old sources and static buffers are charged by actual shared owner,
including caller-retained outputs and uncertain submission quarantine. Acquisition
does not retire the batch; `cancelMatter` drains/removes it before another phase.
The original solve/refine pair is transactionally lazy at first admitted ordinary
prepare; Matter-only initialization loads/reflection-checks the module without
creating that pair. These APIs are a core checkpoint, not completed live-world
scheduling or two-epoch field publication.


### Projected-source native consumer compatibility

Grid's selected-ID optical attachment check also validates the immutable compact
original-ID particle palette once against the actual selected visuals/images.
The qualified coarse A operation is reused; an unavailable particle-only visual
must match an explicitly unavailable canonical source row. It cannot mask known
source transport or yield a participating projected source. Native field binding
requires the exact canonical resident terrain owner plus the unchanged world,
content, terrain mapping, device and material guards. An incorporated Matter
source may precede the live draw's publication/joint tick only within that same
real terrain revision. Static/direct synthetic source contracts remain exact.


### Count-only emitter support screen

Before the first measured nonprovisional Matter source's moments/replay, the same
pass counts conservative support against THAT immutable source's complete S/j
emitter bounds and explicit sky. Receiver1x1 centres are tested only in their
CURRENT page-mask bins; discovered face keys retain the existing one-cell halo
and exactly128 outward bins. Sky, inside and uncertain support include. No mask,
weight, DDA, query count or scheduler is changed.

The existing final discovery key copy is enabled for that measurement. Its exact
completed-fence CPU vector is owned by the immutable result across scratch
cancellation. Device/readback, retained vector capacity, optional returned copy
and old-result overlap are admitted/charged before allocation. A synchronous
optional clock passed from Editor times the count; it is never stored. Count
success, rather than admission/cancellation, marks the once-only observation.
Fixture readback opt-in also exposes the same count value in BounceDemand.

Per-existing-quantum and generation receipts join source/cohort/Matter identity,
window, ordered face-list owner/fence, bounds, counts, bytes and CPU time. Exact
projection/discovery/non-ray quanta, the admitted direct receiver quota times256
directions (capped by page size), and the262144 moment-call ceiling give
only an optimistic required-work lower bound. Raw frame IDs are not cadence;
fixed latency/cadence/GPU calibration remain unmeasured/offline, and no measured
GPU/age PASS is emitted. The existing shared native fixture checks sky inclusion,
a closed-form362/1024 four-cell support count and actual projected-source counts;
all original physical gates and native/secondary transport remain unchanged.


### Parallel direct-window ray execution

`WorldRadiancePass` lazily acquires a private `refineWorldRadianceParallel`
program/pipeline transactionally for direct-window page roots. Admission and
encoding use one64-thread group per receiver; legacy straight/cascade and bounce
remain on their existing pipelines. The shader retains ordered coefficient
projection after parallel ray tracing with4096 bytes of component scratch per
group. Receiver quotas, immutable source leases, overlap copies, fenced
publication and cancellation/quiescence ownership remain unchanged. The shared
native candidate fixture checks reflected64x1x1 entry dimensions and mixed
medium/emission/nonblack-sky all27-coefficient, status and integer work parity;
existing black-sky masking, constant, partial-page and physical controls remain.

Triangle scene readiness uses `TrianglePass::prepare(device, signature)` to prepare
only the distinct variants of currently resident materials, including frames
with no triangle instances. Its transient fixed storage adds no persistent
registry; the cache's fallible const value visitor enumerates live entries and
retains material ownership. Signature/state validation precedes pipeline cache
mutation; preparation reuses ordinary pipeline caching/deferred retirement and
does not upload payloads, rotate slots, submit commands or stamp fences.


`WorldRadiancePass::prepareDiscoveryPipeline()` prepares the optional immutable
slot2 discovery program/pipeline transactionally on the initialized owner thread.
It is idempotent and does not admit buffers, encode, submit or change fence/batch
state. `prepareDiscovery` retains the same lazy fallback after memory admission.

### Persistent temporal world radiance — implementation in progress

`WorldRadianceTransport::temporal_cascades` uses spacing4/six levels by default,
full-world cascade storage and three immutable refined-field slots with weak lease
backpressure. `m_temporal_query_budget` bounds ray work per accepted batch; parent
levels finish all required parities before fine consumers. Positive
`m_temporal_frame_allowance_ms` enables producer-only predictive grouping from
completed per-cascade timestamps; zero retains the fixed-query control. Eight
recent cost samples per class, 8192-query calibration units and at most
2x quota growth are provisional tuning choices. The first page of a compatible
epoch inherits the previous in-budget measured ray count. An overrun or a change
in transport mode clears that seed. Unknown classes calibrate alone. Offscreen
gameplay uses the offscreen cost history and the ordinary mandatory allowance.
Required work targets 92% of the available allowance. The optional rotating tile targets 60%.
Within each cascade, power-of-two query count classes keep short tail dispatches
from inflating the predicted cost per ray for full work units.
Timestamp ranges reserve fixed work-index slots, independently of future batch
boundaries; adaptive `pendingBatches()` is a remaining-work upper bound. Missing
timing support rejects adaptive admission. Overruns and indivisible units that
exceed the allowance are reported. `budgetTemporalFrame()` regroups only the next
unsubmitted adaptive batch; zero returns a defer decision without consuming work,
while negative/nonfinite values and accepted/encoded submissions are rejected.

A native-cell `m_receiver_window` selects complete fine and parent support without
cropping source rays or field storage; an all-zero window selects the full field.
When a GPU-completed empty Matter projection first attaches to the same
published terrain, the pass can copy the certified coefficient and probe-state
buffers to a fresh field slot. This requires matching canonical terrain,
cohort, optical settings, and coverage. The copy retains the transport version
and sample times. The ordinary GPU coverage reduction and a new submission
fence qualify the projected field. A nonempty predecessor or successor, a
changed receiver region, and subsequent feedback iterations take the ray path.

A temporal update can carry one optional refresh tile alongside its primary
receiver window. Each gets its own parent closure. At every level, the planner
subtracts their overlap and encodes only the unique strips, coarsest first.
Both regions share the source, skip table and field preservation. The published
receiver certificate still covers the explicit demand. GPU indirect records use
contiguous row order within a strip. The direct correctness control enumerates
the four spatial parities. Visible and supplemental strips keep distinct GPU timing
histories for each cascade and dispatch size. When a refresh tail has no exact
size sample, its estimate uses a measured dispatch from the same refresh level.
The first unmeasured level is calibrated in isolation. Optional convergence
retains the donor buffers and revisits eligible fine probes. Missing or stale
rows always run, and elapsed time makes a valid quiet row due at 250 ms when
visited. The planner still budgets the full list and preserves the parent
closure. It excludes partially serviced quanta from mandatory ray cost training.
Selectively refined fields expose the checked receiver certificate and an empty
dense updated-probe window.

A replacement admitted after an interrupted temporal page's fence may reuse
that batch's unpublished output slot. Its published donor supplies compatible
history. The ordinary publication and lease rules still apply to every field
that has escaped through acquire.

Partial updates copy retained rows from the previous published donor, or clear
unknown history. A donor must also match the job, world-space origin and metres
per cell, even when the backing allocation dimensions match. An incompatible
lattice starts with unknown rows and cannot contribute multibounce feedback. `updatedProbeWindow()` reports recomputed probe-space coverage;
other rows must not be claimed current for the new source. Editor bootstrap and Matter request visible/gameplay demand; recurring cycles
alternate demand refinement with bounded background tiles, prioritizing missing
coverage. Refresh service-bound and deadline qualification remain incomplete.

The pass owns source-local optional R8UI skip storage, a bounded GPU mandatory
record queue and12-byte indirect arguments. `m_temporal_gpu_queue` selects GPU-built
indices and indirect execution; direct dispatch remains the shared fixture control.
Queue construction and transitions are inside existing stage/batch timestamps and
all buffers/root handles enter admission. Accepted-submit fences retain resources;
discarded encoders do not advance work. GPU constant layout is64B/align4 with
asserted field offsets. Priority/variance bins and complete one-second edit-consumption qualification
remain unfinished; no product-budget pass is implied.

Nonterminal temporal merges select the four-connector parallel shader with
16 records per64-thread group. Terminal and serial-control merges use64 records
per group; parallel refinement uses one record per group. The same admitted
records-per-group divisor governs direct dispatch, GPU indirect arguments and
hardware group-count limits. Ray geometry and ordered floating accumulation stay
unchanged; measured timings, rather than group count, establish the benefit.

Each temporal field slot now owns matching24-byte per-probe state. Published
`transportVersion()` changes conservatively with immutable source identity and is
reused for further samples of the same source. Partial updates copy retained
metadata with coefficients; unknown history remains zero. The injected logical
sample time is stored as two32-bit words, with no shader Int64 requirement.
`readDiagnosticProbeStates()` is guarded by the existing diagnostic opt-in and
reads the completed generation only. Production performs no per-probe CPU readback.
All three state buffers, optional readback/staging and retained-generation owners
are included in admission. At4096² their raw payload adds75,792,672bytes; the
cascade plus three coefficient/state slots total1,133,784,672bytes before sources
and other owners. This is a capacity calculation, not full live-memory qualification.

`m_temporal_coverage_window` independently names the native receiver region to
certify (all-zero means the whole source). The pass expands it to complete fine
interpolation support, runs the GPU missing-version reduction after refinement,
and publishes `currentReceiverWindow()` only when the missing count is zero.
A published field can therefore have empty certified coverage while still retaining
useful history. Same-source retained regions can remain certified across updates
elsewhere; stale-source and unknown probes cannot pass. Presentation compares this
certificate with the current camera/gameplay demand before acknowledging Matter or
first-source consumption, and reports only the certified receiver window.

Cold initialization clears only the dummy parent binding. Required cascade rows
are written in dependency order before use; full-field refinement writes every
published coefficient/state row. Sparse output without a donor clears its own
coefficient/state slot so retained unknown rows are black with zero metadata.
Unused slots and overwritten cascade storage are not bulk-cleared. Feedback reads
are disabled without a published donor; discarded encoders do not initialize the
pool or consume work. This avoids clearing the entire persistent allocation in
one cold batch, without changing allocation ownership or transport calculations.

`GpuFrameTiming::m_surface_frame` identifies the command buffer that owns the
completed timestamp pair: true for `renderAndPresent` (including its preparation
and draws), false for standalone/offscreen `render` and `renderToTexture` work.
The two existing query slots retain this classification through accepted fences;
rejected encoders publish no timing. `lastSurfaceGpuFrameTiming()` independently retains the newest completed surface
sample, even when a newer standalone producer completes. `lastGpuFrameTiming()`
still reports the newest completed submission of either kind. Both observations
reset on timing failure, teardown and device abandonment. This is conservative whole-surface cost, not an isolated
lighting measurement, and it excludes separate producer submissions.

Temporal Matter projection shares the field producer’s frame allowance and
submission token. `encodeMatter` groups adjacent radix digits when the measured
maximum digit time plus 20% headroom fits. A completed projection of the same
terrain and input size can seed both snapshot and digit cost. A measured
snapshot can share its submission with up to eight radix digits when the sum
fits the allowance. Each frozen row carries either a cell key or a drop
sentinel. Radix sorts the bounded rows and preserves snapshot error status. After reduction validates, lookup and comparison can also share a submission
if the compatible previous projection timed both and their sum fits with headroom.
Each dispatch retains its barriers and timing. The complete group checks sticky
status at its accepted fence. The host validates the snapshot before reduction,
and validates reduction before lookup. A zero grouping allowance selects single operations for
standalone callers. The editor defers at zero available time.

For temporal 2x scenes, `GridPass::measureTerrainMaterial` replays the current
indirect terrain geometry into a private RGBA16F MSAA4 stripe. Its viewport has
the original dimensions and its vertical offset selects one sixteenth of the
raster. The material-control word occupies the former reserved word in the
48-byte Grid world-radiance parameters. The control shades contour, texture,
opacity and source while omitting incident radiance and displaced backdrop
reads. It binds a dummy coefficient buffer, so its persistent roots cannot
pin published fields. The visible draw uses its own root and full viewport.

The probe times a zero-area scissor draw and a stripe draw using identical
geometry and bindings. The editor uses their difference for material cost
and charges the full timed calibration pass to lighting. Each completed tile
ring slot owns its scratch target. All three targets together are bounded by
16 MiB and that reserve enters temporal field and Matter memory admission.


An adaptive combined temporal update emits the mandatory dependency closure
first, followed by the disjoint supplemental strips. At an unsubmitted page
boundary the scheduler may end the supplemental suffix and certify the actual
demand. Unvisited fine rows keep their copied values and source versions. A
partially completed tile leaves refresh debt.

A gameplay request can attach one bounded native window. When it is distant
from the viewport, its own coarsest-first dependency closure displaces optional
background work. Both regions feed the GPU coverage reduction; the rectangle
between them consumes no fine samples. The final producer submission copies
the gameplay window's rectangular probe support
row-by-row into the existing fenced readback. The pass publishes a CPU snapshot
only after the GPU reports complete coverage. Bilinear sampling blends all 27
signed coefficients in world coordinates. Caller-retained snapshots are charged
on later admissions independently of the generation that originally owned them.

The temporal transport accepts an optional integer solar hour as boundary
illumination. An hour change reuses the compatible probe lattice and field
slots but assigns a new transport version; CPU and shader coverage checks
then require every demanded probe and parent on that version. Equal hours can
continue refining the existing version. Published gameplay rows and the
generation retain the sampled hour. The prospective light bound includes one
unit of RGB environment radiance and its additional underflow allowance.
