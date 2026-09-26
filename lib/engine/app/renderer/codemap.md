# lib/engine/app/renderer

## Responsibility

The `engine.app:renderer` module provides the content-free generic `Renderer` (surface registry + graphics
queue + validated submission only; multi-surface capable, single main window in production). Scene content lives in `engine.app:renderer.triangle_pass` (`TrianglePass`);
camera-free RHI-facing submission shapes (`RenderPipelineSignature/Key`, `DrawContext/Callback/Submission`,
`ColorAttachmentOps`, `ESurfaceDepthPolicy`, `SurfaceRenderPass`) live header-only in
`engine.app:renderer.types`. There is no `App.Renderer.Types.cpp` — the partition is fully inline in
`App.Renderer.Types.cppm`.

## Design

- **Renderer** (`App.Renderer.cppm/.cpp`): config `m_preferred_surface_format` (default Undefined → backend
  preferred), `m_desired_image_count{3}`, `m_enable_vsync{true}`; state `FlatMap<WindowHandle, SurfaceRecord>` +
  `m_graphics_queue` + `safe_ptr<IRhiService>` + `m_owner` thread id. `SurfaceRecord` owns `ISurface`, a resize-matched D32Float
  depth texture/view, last `m_extent`, and `m_configured`. The depth pair is shared only by passes that explicitly
  select `ESurfaceDepthPolicy::renderer_owned`; it is not globally attached to presentation. The renderer is
  render-thread confined like the pass caches: `initialize` captures the owner thread, surface and presentation
  paths fail closed (`operation_not_permitted`) off-thread, and `shutdown` clears the owner.
- `initialize(rhi_service)` grabs the Graphics `ICommandQueue` only and captures the owner thread;
  `shutdown()` retain-first-error teardown: off-thread call fails closed (`operation_not_permitted`), else
  `waitOnHost`, unconfigure every configured surface, clear map/queue/service refs and the owner.
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
  Offscreen callers must `waitOnHost()` before readback.
- `renderAndPresent(window, passes)` is the single-pass convenience API: it acquires one surface image and renders
  the draws carried by each `SurfaceRenderPass` in order before one present. `SurfaceRenderPass` holds its own
  `m_draws` (`std::initializer_list<const DrawSubmission>`) plus its color/additional-color attachments and depth
  policy, so each pass independently selects no depth, the renderer's resize-matched shared depth view
  (`ESurfaceDepthPolicy::renderer_owned`), or an external depth descriptor (`external`). This permits the 3D pass
  to use depth while a following ImGui pass loads color without any depth attachment, or a later compatible pass to
  select a different external view/format without pretending the backend supports different depth views inside one
  native pass. Policy/descriptor mismatches and empty pass lists fail before image acquisition; an acquired
  backbuffer whose extent no longer matches the resize-matched depth view fails (`invalid_argument`) before
  pass building. There is no
  `renderAndPresentPasses` and no `SurfaceDrawPass` type. Recording is **two-phase over one shared ScratchPad
  scope**: phase 1 builds every `BuiltSurfacePass` (color attachments, depth attachment, `RenderPassDesc`, then
  `surfaceSignature_` → signature + reference extent) into a reserved `Array`, and the first failure stops the
  build; phase 2 runs only on success, creating one encoder, encoding the already-built passes, then one
  finish + submit — every failure is retained so the acquired image still presents, and a failed pass abandons the
  encoder without finish/submit while `present()` still runs.
- `surfaceSignature_(record, pass, desc, out_signature, out_extent)` is the surface-hoisted signature build. The
  depth policy picks one of two `SurfaceRecord::m_signatures` slots (`none` = 0, `renderer_owned` = 1); external
  depth and any pass with `m_additional_colors` is caller-shaped and never cached. A hit reuses the stored
  signature only when the slot is valid, the depth policy still matches, and the stored extent equals the record's
  current extent (compared componentwise); otherwise `buildRenderSignature_` runs and repopulates the slot.
  `resizeWindowSurface_` clears both slots right after its thread check, so a resize always rebuilds.
- `createWindowSurface_` validates out-param/service/native/handle, `createSurface(fromHwnd(native))`,
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
  `g_payload_base` is bound per group and `startInstanceLocation` is 0, because `SV_InstanceID` is draw-local on
  D3D12, SPIR-V, and Metal alike. Failure is fail-closed but not all-or-nothing: resolve and payload upload are
  all-or-nothing, while encoding stops at the first group that cannot be encoded and prior groups stand.
  There is no indirect or compute-publish lane.
  **Payload ring**: `kPayloadRingSize{3}` (mirrors `m_desired_image_count`) upload buffers at `InstancePayload`
  stride, each created and mapped once in `ensureDirectPayloads_` and kept mapped for its whole life;
  `uploadPayloads_` rotates `m_payload_ring_cursor` once per submit and writes through the persistent mapping
  (no per-frame map/unmap), and `encodeGroup_` binds the current slot. `releaseDirectPayloads_(device)` drops the
  whole ring — unmap only when a device is supplied (growth/replacement), `nullptr` from `shutdown()`/
  `notifyDeviceLost()`, which is safe because `SLANG_RHI_DEBUG_ENABLE_BUFFER_MAP_VALIDATION` defaults to 0.
  `pipelineFor_` validates the signature shape (one color format — the sample count is a `SampleCount`, so every
  value is accepted by construction) BEFORE the cache clear,
  so an unsupported signature can never drop the pipelines built for the last good one.
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
  a surviving scene's refcounted texture entry. Material slots never recycle and
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
2. Per-frame → stack 3D and UI `DrawSubmission` arrays → two `SurfaceRenderPass` records →
   `renderAndPresent` (resize on drift → acquire → depth-enabled 3D render pass → color-load/no-depth UI
   render pass → present); `TrianglePass::update(snapshot)` then `TrianglePass::render` inside the 3D callback.
3. Offscreen/tests → `renderToTexture(target, …)` → `waitOnHost()` → readback
4. On minimize/resize-to-zero → surface unconfigures but the record stays; next non-zero frame reconfigures
5. Shutdown → `triangle.shutdown()` → `destroyWindowSurface(*main_window)` → window destroyed via the window
   service → `renderer.shutdown()` (wait, unconfigure any survivor, release)

## Integration

- **Consumers**: owning application shell (owns `Renderer` + `TrianglePass`, drives update/render/shutdown)
- **Depends on**: `engine.core`, `engine.math`, `engine.rhi` (devices, queues, surfaces, passes, pipelines),
  `engine.shader` (TrianglePass program load), `engine.image` + `engine.mesh` (cache upload types only),
  `:service.window` + `:window.handle` (Window resolves surfaces),
  `:scene.camera` (TrianglePass `CameraSnapshot` only — never a mutable `Camera`)
- **Provides**: `engine.app:renderer`, `engine.app:renderer.triangle_pass`, `engine.app:renderer.types`
  (camera-free boundary: passes consume `DrawContext`/snapshot data while drawing)

## Key Files

- `App.Renderer.cppm` — generic `Renderer` declaration (config, surface registry, render/renderToTexture/renderAndPresent/waitOnHost/destroyWindowSurface)
- `App.Renderer.cpp` — Renderer implementations (attachment inspection/validation, encode/submit, surface create/resize/destroy, retain-first-error shutdown)
- `App.Renderer.TrianglePass.cppm` — `TrianglePass` declaration (FrameConstants layout, snapshot cache, caches, pipeline helpers)
- `App.Renderer.TrianglePass.cpp` — TrianglePass implementations (invariant state, shader program, variant pipelines, resolve/plan/upload/encode, frame-constant upload)
- `App.Renderer.GpuCaches.cppm` — pass-owned cache vocabulary (handles, `TriangleBagRange`, `ResolvedBag`, `GpuMaterial`, caches)
- `App.Renderer.GpuCaches.cpp` — cache implementations (uploads, dedup, pack, teardown)
- `App.Renderer.Types.cppm` — boundary types (`RenderPipelineSignature/Key`, `DrawContext/Callback/Submission`, `ColorAttachmentOps`, depth policy, `SurfaceRenderPass`); header-only, no matching `.cpp`
