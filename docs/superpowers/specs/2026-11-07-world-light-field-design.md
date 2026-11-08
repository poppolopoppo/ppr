# World light field — simplified emissive temporal radiance cascades

Status: design draft for user approval. Supersedes the receiver-window,
publication and deadline-scheduling model in
`2026-11-06-emissive-temporal-rc-pivot-design.md` and
`2026-11-06-spacing4-adaptive-rc-budget-design.md` for the lighting transport.
Those documents remain historical evidence for the abandoned approach.

## Intent and why

PPR should render its colony with an ONI-v22-style lighting model: emissive
world cells and materials are the light sources, an optional sky contributes at
the terminal cascade boundary, and one world-space radiance-cascade field is the
only production illumination solution. The field is temporally amortised and
accumulates samples over time; because probes are fixed in world space there is
no reprojection and no camera-coupled history. Surfaces are shaded by the normal
triangle/indirect-draw scene path, not by a prototype-style full-screen compute
ray march.

The previous implementation solved a harder problem than the product needs. It
started from a 4096² shipping gate, a camera-receiver-window demand model and a
sub-second publication deadline, and grew four transports, two complete solvers,
three temporal resolvers, five state machines and a 7,592-line pass. That design
is now blocking work: camera pan/zoom can terminate the application, normal play
logs continuously, and edits take 0.8–1.8 s. This design keeps the parts that
earned their place (frame/lane/fence ownership, the indirect terrain draw lane,
the RetroPass post chain, native DDA material traversal) and replaces the rest
with the prototype's model.

## Goals

- One production lighting path: world emissives plus optional last-cascade sky,
  transported by a persistent world-space radiance cascade field.
- Camera pan and zoom never change, invalidate or gate the field. Moving the
  camera may only change what is rasterised and the screen reconstruction
  history.
- A world edit becomes visible through the normal refresh cycle, not through a
  receiver-window admission, publication or deadline protocol.
- Temporal supersampling per probe in world space: jittered angular and positional
  samples accumulate while the world is static; edits restart the affected
  probes.
- Approximate multiple bounces through previous-field feedback: when a traced ray
  hits a surface, the hit shading adds the material response of the previous
  completed field at that hit. One previous-field read per hit, no explicit
  recursive ray tree, no face/moment bake and no per-frame path tracing. This is
  the prototype's `RadianceMultiBounce` model and the previously approved
  feedback intent; it is not replaced by EMA alone.
- Terrain and other opaque content are shaded by the normal triangle fragment
  path with indirect draws, sampling the field per fragment.
- A post chain of eye adaptation, bloom and AgX tonemapping on an HDR scene.
- Default map 2048², baseline 1024², 4096² is an aspirational benchmark only and
  is not an acceptance gate.
- Initial refined-field spacing is 2 native cells at every map size, including
  2048². Accept the prototype's small-feature leaks for now.

## Non-goals

- No 4096² shipping guarantee, no 10 ms stress gate and no receiver-window
  coverage certificate.
- No analytic lamps, light lists, direct-light pass or explicit face/moment
  bounce bake.
- No full-screen compute ray march as the production surface renderer.
- No per-cell geometry. Terrain is never one mesh or one quad per cell.
- No screen-space GI correction pass in the first implementation. It may return
  later only for a measured visible-artefact need.
- No RHI migration and no new third-party dependency.
- No gameplay illumination query protocol in the first implementation. The
  current query is a logging observation, not a mechanic; a future mechanic gets
  a direct sample from the field owner.

## Anti-goals

- A design that only passes by disabling validation, shrinking map size silently,
  or replacing the field with a reference tracer.
- A design whose normal play performs per-frame CPU readbacks or logs.
- A design that keeps two production solvers and selects between them at runtime.
- A design that reintroduces camera demand rectangles in a different name.

## Constraints

- Existing engine contracts: `CameraSnapshot` is camera-free at pass boundaries;
  row-major/row-vector/+Z-forward/+Y-up/[0,1] depth; `Renderer` owns content-free
  frame orchestration; the render lane owns GPU submission and `FrameFence`
  settlement.
- The game camera is orthographic and plane-parallel on the map plane. Zoom is
  presentation-only for lighting.
- World sizes: 1024 (baseline), 2048 (default/maximum), 4096 (benchmark).
- Spacing 2 cells; cascade base directions 4; cascade levels derived from map
  size and far interval.
- Output: 3840×2160 from a 1920×1080 internal raster, MSAA4 and temporal 2×
  reconstruction retained unless measurement shows they must change.
- 60 Hz target at 1024² and 2048². Quality wins over spacing: if a frame budget
  is exceeded, lengthen the refresh cycle before coarsening the field.
- Time is injected; no implicit clocks in the field or pass.

## Approaches considered

### A. Full-map persistent field with triangle/indirect surface shading (chosen)

One field per generated world, covering the whole map, updated round-robin,
camera-independent. Terrain is drawn by the existing indirect tile-quad lane and
shaded in the fragment stage against the field. The renderer keeps its frame,
fence, resolve and post infrastructure. The publication, receiver-window,
readiness, deadline and gameplay-query machinery is deleted.

Why: it is the prototype's model, it removes the structural cause of the camera
crashes and edit latency, and it deletes far more code than it adds. Fixed
world-space probes make temporal supersampling trivial because no reprojection
is required.

### B. Prototype-style full-screen compute world shading

A full-screen or half-resolution compute `shadeWorld` that ray-marches the world
per pixel and samples the field, with UI and particles composited on top. Closest
to the original prototype, but it discards the engine's triangle pipeline and
scene integration, makes 4K output expensive, and does not fit the request to use
the normal triangle/indirect path. Rejected.

### C. Harden the current receiver-window architecture

Patch the concrete failures (retain busy/plan errors, clamp zoom, gate logs) and
keep the demand/publication machine. Smallest diff, but retains the five state
machines and the camera-coupled field that are the actual blocker. A shorter path
to a stable demo, but not to a maintainable implementation of the stated goal.
Rejected.

## Design

### Ownership and frame flow

```
simulation snapshot / world edit
        |
        v
WorldLightSourceGpu   dense cell ids + material palette + Matter overlay
  (dirty-rect upload, one source revision)
        |
        v
WorldLightField       cascade + refined probe buffers, probe state, work cursor
  (coarse-to-fine temporal update, round-robin, one frame submission)
        |
        +--> GridPass indirect tile draw -> fragment shading samples the field
        |
        +--> (optional later) gameplay/CPU sample of a probe
        |
        v
RetroPass  MSAA4 resolve -> temporal2x -> bloom -> eye adaptation -> AgX -> present
```

- `WorldLightField` is owned by the render lane and is touched only by its owner
  thread. It has one `prepare/encode/advance` style API only if the existing lane
  needs it; it has no leases, generations, publication slots or receiver windows.
- The field update is encoded into the same frame submission as the scene draw,
  before the scene pass. The fragment shaders read the field after a queue
  barrier. There is no multi-frame publication.
- `WorldLightSourceGpu` is updated in the same owner lane from the committed
  world snapshot and the committed Matter projection. Dirty rectangles are
  uploaded with the frame's existing upload ring semantics.
- The field is a live, always-present buffer. Consumers never ask whether a
  region is "admitted"; they sample the current field. Freshness is a quality
  property, not a correctness gate.

### World source

- `WorldLightSourceGpu` owns a dense native-cell buffer (`u16` id per cell), one
  **global material palette indexed by element id** with the optical terms the
  solver traces (opaque/absorbing/vacuum kind, extinction, volume emission,
  surface emission), a dense Matter overlay buffer and a small occupancy pyramid.
  It replaces the CPU `WorldRadianceSource` capture in the production path.
- "Per-chunk cell materials" is only the upload granularity inherited from the
  simulation. The production storage is one global per-cell id buffer plus the
  global palette; a chunk upload copies its ids into that buffer. Material
  properties are never duplicated per chunk or per cell.
- **Edits consume the committed dirty set, not a full-world capture.** The
  existing `GridStagedFrame` already carries per-chunk `m_cell_materials` and a
  dirty mask (`GridUploadRequest` / `GridTileSubmission`). For each dirty chunk,
  copy its 128² ids into the dense buffer at the chunk offset and patch the
  occupancy bitset. A full upload happens only on world generation or catalog
  change. The frame path must stop constructing a full `WorldRadianceSource` per
  edit.
- **Revision and dirtiness.** A single `sourceRevision` increments when the
  committed terrain, Matter overlay or sky changes. A dirty cell/chunk rectangle
  (union of changed regions) is carried for probe reset/prioritization, but
  correctness never depends on it: a missed dirty region only delays convergence
  through the normal cycle. No publication, lease, generation or coverage
  certificate exists.
- **Matter.** Replace the six-stage `world_matter_project.slang` pipeline
  (radix rank/prefix/scatter + group reduce + hash lookup/ancestor build +
  old/new diff) with a dense overlay projection:
  1. clear the dirty cell region (or use a generation tag),
  2. scatter each committed particle's fill/coverage-weighted optical terms into
     its target cell with atomics,
  3. finalize the dirty rows (normalize opaque coverage, clamp, merge terrain
     optics in the field tracer or a finalize pass),
  4. update the dirty rectangle and the occupancy pyramid from the same rows.
  The dense overlay is indexed by cell, so no hash lookup, ancestor buffer,
  sparse-row list or radix sort is needed. The overlay is part of the same source
  revision. Flat float atomics make the order non-deterministic; that is
  acceptable for the field approximation and tests use tolerances. If a future
  gate requires bit-exact ordering, a deterministic variant can be added then.
  The particle render path remains separate and is clipped by terrain as
  described above.
- **Sky** is a small uniform (hour-derived colour). It is not an analytic light;
  the coarsest cascade reads it. An hour change bumps the revision and dirties
  the coarse probes only.
- **Synchronization.** The producer commits the world/particle stage before the
  render lane consumes it. The source upload is encoded in the render lane before
  the field update in the same submission; the field update reads the uploaded
  source after a barrier. There is no cross-frame source ownership, no source
  lease and no readback.
- **Failure handling.** An upload or projection failure returns the ordinary
  error path, retains the previous source bytes and revision, logs once per
  distinct failure and retries on the next committed frame. It does not latch a
  permanent failure state.
- The CPU `WorldRadianceSource` may remain as a test/tool oracle. It is not
  captured or uploaded per edit in the frame path.
- Indicative source memory at 2048²: dense ids ~8 MB, material palette small,
  overlay ~16–64 MB depending on layout, occupancy ~1 MB. At 4096² multiply the
  dense terms by four.

### Field layout

Follow the radiance-cascades hierarchy directly. There is **one cascade
hierarchy**; there is no separate refined CH field and no angular-bin probe.

- Level 0: probes spaced 2 native cells, 4 directions.
- Level `l`: probes spaced `2^(l+1)` cells, `4^(l+1)` directions
  (4, 16, 64, 256, ...). Levels continue until the coarsest spacing covers the
  far bound.
- Each level stores radiance for **its own interval**
  `[start_l, start_(l+1))`; the coarsest interval extends to the far bound and
  merges the sky. Levels are not merged down into level 0.
- Rationale: near occluders subtend large angles, so the 2-cell/4-direction
  level already has enough angular resolution. Distant occluders subtend small
  angles, and the coarser levels carry the higher direction counts. Shading
  composites the hierarchy so each distance range keeps its own angular
  resolution.
- Per angular sample: RGB radiance plus a scalar transmittance term, packed to
  8 bytes. Scalar transmittance is the transport approximation; the radiance
  remains RGB. A per-direction sample count (u8 saturating) and a per-probe
  deterministic phase are kept separately. No CH coefficients, no 9- or
  16/32-bin representation.
- Indicative storage, double-buffered radiance (8 B/sample):

  | Map | Level 0 | Six levels | Double-buffered | Counts/state |
  |---|---|---|---|---|
  | 1024² | 8.4 MB | ~50 MB | ~100 MB | ~3 MB |
  | 2048² | 33.6 MB | ~202 MB | ~404 MB | ~12 MB |
  | 4096² | 134 MB | ~805 MB | ~1.6 GB (aspirational; use coarser spacing or 16-bit radiance) | — |

  These are field-only bytes; scene targets, source buffers and normal renderer
  allocations are separate.
- Traversal acceleration over the dense cell source uses a small occupancy
  pyramid built on the GPU (for example one bitset level at 8×8 or 16×16 blocks,
  optionally one coarser level). Dirty-rect edits patch or rebuild only the
  affected blocks; no CPU-side `OpticalHierarchy` capture is maintained per
  edit. Block sizes and level count are builder-measurable constants; start
  simple and add a level only for a measured traversal bottleneck.
- Work list: a simple deterministic cursor plus a dirty-probe list is enough.
  A GPU-built indirect work list is allowed only after the simple version is
  measured and the remaining cost is in dispatch setup.

### Temporal supersampling

The field is static in world space, so every probe direction integrates samples
over time without reprojection. There is no CH projection and no screen-space
history.

- **Per-direction accumulation.** Each probe stores one running radiance mean and
  a sample count per direction. An update traces one or a few directions of a
  selected probe over that level's interval and blends the new sample into the
  direction's mean. Directions are independent, so partial updates are natural.
- **Angular and positional jitter.** A direction's sample is jittered inside its
  angular bin, and the probe origin is jittered inside its footprint. Each probe
  owns a deterministic low-discrepancy phase (hash plus golden-ratio step), so
  adjacent probes do not sample the same directions on the same frame. This is
  the prototype's world-space supersampling idea applied per direction.
- **Accumulation.** While `count < N_max` (initial 8–16 per direction), blend
  `mean += (sample - mean) / (count + 1)`. After `N_max`, blend with a fixed
  retention per 60 Hz interval (initial 0.9–0.95). The sample count is per
  direction; a probe is considered converged only when its directions are
  converged.
- **No reprojection.** No camera-dependent history, no velocity, no TAA clamp.
  Camera cuts, zoom, resize and screen-history resets have no effect on the field.
- **Levels are independent.** A level traces only its own interval; it does not
  read or merge its parent. The coarsest level extends to the far bound and adds
  the sky term. This keeps levels parallel and avoids a coarse-to-fine dependency
  chain; shading performs the near-to-far composite.
- **Multi-bounce feedback.** The hierarchy is double-buffered (previous/current).
  Hit shading during an update reads the previous hierarchy plus the current
  source. Buffers swap after the update. Because each level's stored radiance is
  interval-local, the feedback read composites the previous levels in the same
  way the shading path does.
- **Edits.** Probes intersecting the changed cell rectangle are added to a dirty
  list and their direction means/counts are reset in both buffers, for every
  level whose footprint intersects the rectangle. Unrelated probes keep history.
- **Sky.** Only the coarsest level carries sky. An hour change dirties the
  affected coarse probes; the normal cycle converges them and never resets the
  whole field.
- **Convergence target.** A 4-direction level-0 probe can update all directions in
  one turn; with a 16-frame cycle and `N_max = 8–16` per direction, convergence
  is a fraction of a second at 60 Hz. Dirty probes can use a larger batch for a
  faster visible response. Cycle length and per-turn direction count are the
  primary quality/performance levers.

### Update scheduling

- **Work unit:** `(level, probe, direction)`. A direction sample is one ray over
  that level's interval plus the per-direction accumulator update. Level 0 probes
  have 4 directions; each coarser level has 4× more directions and 4× fewer
  probes, so every level has the same number of direction samples.
- **One dispatch per level per frame**, six dispatches total, recorded before the
  scene draw in the same submission. A level's dispatch consumes a contiguous
  range of its deterministic cursor; no cross-level dependency exists because
  levels store interval-local radiance and shading composites them.
- **Dirty first.** A dirty rect is expanded to each level's probe footprint and
  produces a small dirty work range. Dirty ranges are consumed before the
  round-robin cursor. A typical edit (one 128² chunk at level 0) is 4,096 probes
  × 4 directions = 16,384 samples, so it converges in one or a few frames even
  with a modest budget.
- **Budget.** A per-frame sample budget is exposed as a quality setting and tuned
  from measurement. The starting target is a 6 ms lighting GPU budget at 60 Hz
  for both 1024² and 2048². If the budget does not cover the desired cycle, the
  **cycle lengthens**; the field never silently coarsens spacing, changes solver,
  or drops dirty work.
- **Indicative cycle lengths:** at 2048², the whole hierarchy is
  ~25 M direction samples. With a 6 ms budget and a measured per-sample cost, the
  initial target is a 16-frame cycle (~0.27 s at 60 Hz); if measurement says
  otherwise, a 24–32 frame cycle is acceptable. At 1024² the hierarchy is
  ~6.3 M samples, targeting an 8-frame cycle.
- **Work list.** Start with CPU-built contiguous ranges per level and one indirect
  or fixed-count dispatch per level. Move to a GPU-built work list only if
  measured dispatch setup dominates; the simple path is preferred.
- **No per-frame GPU readback** is required for correctness. Optional per-level
  timestamps are opt-in, sampled at a low cadence, and never gate presentation or
  correctness. Do not resurrect a predictive deadline scheduler.
- **Progress guarantee.** Every direction is visited within the cycle; dirty work
  may preempt the cursor but cannot starve the background refresh indefinitely
  (bounded dirty share per frame, with the remainder deferred).
- **Over-budget behavior** is graceful: the frame remains presentable with the
  previous field contents; only convergence is delayed.

### Shading through the triangle/indirect path

#### Surface geometry and scaling

The renderer keeps the current tileset geometry model. It does **not** build one
mesh, quad or triangle pair per native cell.

- One quad (six non-indexed vertices, two triangles) represents a whole
  128×128-cell chunk, exactly as `vertexGridMain` does today
  (`kGridTileEdge = 128`). Chunks are culled and drawn through the existing
  indirect lane.
- The fragment stage reconstructs the native cell from the fragment's world
  position using the existing chunk atlas, then reads the cell material/emission
  and samples the light field. Shading cost is therefore proportional to visible
  raster pixels, not to map cell count.
- Geometry counts at maximum zoom-out (whole map visible) are trivial:

  | Map | Chunks per edge | Quads | Vertices |
  |---|---|---|---|
  | 1024² | 8 | 64 | 384 |
  | 2048² | 16 | 256 | 1,536 |
  | 4096² | 32 | 1,024 | 6,144 |

  Fragment work stays at the internal raster size (1920×1080 by default) at every
  map size. Zooming out changes coverage and cell-detail frequency, not the
  number of shaded pixels.
- Chunk LOD (drawing larger quads when zoomed out) is unnecessary at these counts
  and is not part of the design. If a measured problem ever appears, the first
  response is chunk-size tuning, not per-cell geometry.
- Keeping the chunk quad lane also preserves chunk-local atlas locality, chunk
  upload/dirty tracking, culling and the existing particle/fluid draw paths. The
  alternative of one full-screen triangle plus a dense world lookup is simpler in
  geometry but discards those existing advantages for no measured gain; it is not
  chosen.
- Translucent/fluid surfaces keep a separate simplified blended path; the opaque
  field-shaded pass remains the main terrain path.

#### Opaque shading

- Terrain and other opaque world surfaces continue to be drawn by the existing
  indirect tile lane (`GridPass::renderGpuIndirect`, six vertices per chunk quad,
  culling and chunk atlas uploads). This is a normal vertex/fragment pipeline,
  not a compute ray march.
- Material lookup stays `cell id → material row`. The fragment keeps the albedo
  texture and the normal map, sampled with explicit gradients and mipmaps. It
  drops the texture-patch barycentric/variance machinery, cut-face incident term,
  refraction displacement and backdrop reads in the first cut. The retained
  material terms are albedo, emissive × strength, opacity, roughness, metallic,
  IOR and transmittance.
- Emission is linear and identical in the field source and in the fragment. The
  fragment adds self-emission once; the field supplies incident radiance only.
  The old display-only `1-exp(-emissive·80)` glow is removed.
- Per-cell mass/temperature modulation is not part of the first cut; the cell
  atlas carries only the material id. The source format can add it later without
  changing the field.
- One shared shading body replaces the current `Main/Spatial/Reactive` code
  duplication and the `grid_screen_contact` variants. The thin entry wrappers
  required by the existing pipelines remain: the plain MSAA entry, the
  `SV_SampleIndex` spatial entry using `EvaluateAttributeAtSample`, and the
  reactive entry that also writes the R8 reactive mask consumed by
  `temporal_2x.slang`. The wrappers call the same body; the pipeline target-count
  and sample policy still select them. Losing the reactive entry would cause
  history ghosting after edits and camera motion, so it is retained.
- The body:
  1. resolves the native cell and material from the existing cell atlas;
  2. evaluates the cascade composite at the fragment's world position for a
     direction `d`: sample each level's directional radiance bilinearly at that
     position, then composite near to far with transmittance,
     `L(d) = L0(d) + T0(d)·(L1(d) + T1(d)·(L2(d) + …))`. This is what lets a
     4-direction level 0 stay sufficient: far intervals keep the finer angular
     resolution of the coarser levels.
  3. for diffuse, integrates `L(d)·cos` over a small deterministic direction set;
     for specular, evaluates `L` at the reflection direction (optionally with a
     small roughness lobe). The retained material terms (albedo, metallic,
     roughness, IOR) weight those terms. Self-emission is added once.
  4. writes scene-linear HDR.
- Base surface normal is +Z. When a material has a normal map, the fragment
  samples it with explicit gradients, decodes tangent space, scales `xy` by the
  material normal scale, clamps `z` to a small positive value and normalizes;
  the world tangent frame is simply world X/Y/Z because the terrain is the
  `z = 0` plane. Normal maps are retained because they make the directional
  response visible and readable; without them the top-down view would mostly show
  the diffuse near-isotropic term.
- A per-cell open-edge macro normal is a planned follow-up, not a first-cut
  item: it is important for gameplay readability so lit areas reveal the cells
  around them. The extension is a neighbour-occupancy normal blended under the
  detail normal; a full chamfer/height-field treatment is not required. The field
  and material shading must not assume the base normal is always +Z, so this can
  land without changing the solver.
- No ambient-occlusion term is added in the first cut. The field supplies the
  occlusion/incidence that matters; a cheap neighbourhood or screen AO can be
  evaluated later on captures.
- Particles and fluids keep their existing draw paths and may sample the same
  field. Matter contributes to the source overlay; it is not a separate light
  source.
- No screen-space world-GI correction, backwall receiver correction or
  receiver-window sampling is performed. The field is the sole world
  illumination solution for opaque surfaces.
- `TrianglePass` remains the path for imported meshes/props and is not replaced
  by this work.

#### Translucent and fluid shading

- Translucent terrain cells (water, oil, gases, glass, gems) render in a second
  blended pass over the same chunk geometry. The opaque pass writes depth; the
  translucent pass tests depth but does not write it. The 2D world is a single
  depth layer, so no per-cell sorting is required; fluids and particles compose
  in their existing overlay order.
- The translucent fragment uses the same material lookup, normal map and field
  sampling as the opaque path. It outputs field-shaded albedo plus self-emission.
  Emission is still added once.
- **Refraction and coloured absorption are retained** because they are what makes
  water, glass and crystals readable. The fragment keeps the IOR/normal-based
  backdrop offset and a Beer–Lambert tint from the material absorption over a
  fixed thickness. Refraction samples the background/backwall copy, not the
  already-rendered scene: in this 2D cell model a translucent cell has the
  background behind it, and particles are explicitly clipped to non-opaque cells
  (below), so there is no terrain behind a translucent sample. A scene-colour
  refraction copy is not introduced.
- Fluids and particles keep their existing instanced-quad draw and surface
  reconstruction paths (the liquid surface reconstruction is visually useful),
  sample the same field, and use the same optical backdrop. They do not get a
  second lighting model.
- **Particles and fluids are clipped by terrain.** The fragment resolves the
  underlying native cell with the same cell lookup/stencil as the terrain; a pixel
  whose native cell is opaque terrain discards. Coverage antialiasing is allowed
  only on the geometric boundary, never over a solid cell. This is a hard
  contract, not an emergent result of draw order.
- Pass order: background/backwall → opaque terrain (depth write) → particles and
  fluids (depth test, no depth write, clipped to non-opaque cells) → translucent
  terrain cells (depth test, no depth write, blended over particles). This keeps
  particles under water/glass while never letting them paint over solid terrain.
- Translucent pipelines use the same plain/spatial/reactive entry wrappers as the
  opaque path, with blending and depth-write state set by the pipeline.

### Post chain

Order: scene HDR → spatial resolve → temporal 2× resolve → luminance reduction
→ eye adaptation → bloom (exposure-aware threshold) → present (bloom add,
exposure, AgX, optional palette/dither, sRGB).

- **Scene target:** RGBA16F HDR, MSAA4, existing spatial and temporal 2× resolve.
  Temporal history stays scene-linear; exposure is not baked into history.
- **Eye adaptation via luminance histogram:** downsample the resolved HDR scene
  to a small luminance image (for example 64×64), then build a log-luminance
  histogram on the GPU (64–128 bins, workgroup-local histograms merged into one
  global histogram). Reduce the histogram to the target exposure using a robust
  statistic (weighted average between percentile cutoffs, starting around the
  10th/90th percentiles), not a single average that black or one bright emitter
  can dominate. A 1×1 temporal state adapts the EV with the injected delta and a
  ~0.7 s time constant, clamped to ±4 EV around the manual exposure. Start
  symmetric; fast-bright/slow-dark is a tuning option.
- **No exposure readback.** The histogram, target EV and adapted EV all stay on
  the GPU. The adapted EV lives in a small texture/buffer sampled by the bloom
  threshold and the present shader. The CPU only sets the manual EV offset and
  the enable/disable flag.
- **Bloom via SPD-style mip reduction:** replace the current quarter-res extract
  plus binomial blur with a modern bloom chain:
  1. exposure-aware bright pass with a soft-knee threshold and Karis weighting at
     half or quarter resolution;
  2. a single-pass downsampling chain in the style of AMD's single-pass
     downsampler (group-shared 2×2 reduction generating several mips per
     dispatch, with correct handling of odd extents);
  3. a tent/bilinear upsample chain that accumulates each level back into the
     previous one, so the final bloom is a wide, smooth, energy-conserving
     result;
  4. one additive bloom sample at present.
  Use FP16 bloom targets, minimize dispatch count and bandwidth, and keep the
  chain power-of-two friendly. The histogram downsample and the bloom bright pass
  may share a raw scene reduction if measurement shows the extra read is
  material; otherwise keep them separate for clarity.
- **AgX:** keep one AgX implementation (`display_agx.slang`) and one look choice;
  no second tonemapper.
- **Palette/dither:** becomes an optional look, default off in the new path so
  AgX is the visible tonemap. The retro palette can remain selectable.
- **Fixed-exposure override:** captures and deterministic image tests disable
  adaptation and use a fixed EV. Adaptation convergence, clamp and black-frame
  behavior get their own focused checks.

### Camera and application stability

- Remove all lighting receiver demands and window planning from the frame path.
  Camera position, ortho scale, FOV and resize cannot produce a lighting error.
- The ortho zoom limit is clamped to the map extent for presentation so the view
  cannot select an unbounded empty region. Invalid viewport extents reuse the
  last known extent for camera bounds.
- Lighting update failure retains the last completed field contents and logs
  once per distinct failure. It is not a frame-fatal error unless it is a real
  device/loss or lifetime failure.
- Per-frame measurement logging is off unless explicitly enabled. Admission,
  drop, deadline and coverage logs disappear with the machinery that produced
  them.

### Error handling and lifetime

- The field owner is the render lane. All GPU resource creation, update,
  submission and release happen on that lane.
- One field and one source buffer per world generation. Shutdown drains the
  normal frame lanes and releases resources in reverse order; it does not need a
  quiescence proof for a multi-slot publication because there is no publication.
- A source upload failure or a field dispatch failure returns the existing
  `std::error_code` path, retains the previous field contents, and is retried on
  the next frame. It does not latch a permanent failure state.
- `std::bad_alloc` remains fatal at the application boundary.

## Deletions and retirements

Delete or retire, in the same change series as the replacement:

- `App.Renderer.LightingPass` / `RadianceCascadePass`,
  `App.Renderer.OpticalGeometry`, `radiance_cascades.slang`,
  `radiance_cascades_refined.slang`, `radiance_terrain.slang`,
  `terrain_query_shared.slang`, `terrain_precision*.slang`,
  `terrain_receiver_types.slang` (roughly 9k lines, test/offline only).
- The four world transports, face discovery/moments/bounce, skip-table
  experiments, serial reference entries, receiver windows, publication slots,
  leases, deadline prediction, coverage certificates, gameplay query
  receipt/field protocol and their tests.
- The six-stage Matter projection shader and its host stages (snapshot/rank/
  prefix/scatter/reduce/finish), replaced by the dense overlay scatter.
- `grid_screen_contact*`, backwall receiver correction, `temporal_resolve.slang`
  and the unused resolve variants.
- Legacy test tiers: `App.RadianceGpu.Tests.cpp`, the receiver/publication/
  versioning/skip-table/face/bounce/screen-contact cases in
  `App.WorldRadiance.Tests.cpp`, and the unrelated precision suites
  (`fp64_runtime_arithmetic`, `packed_*`, `terrain_precision_*`,
  `terrain_receiver_continuation_experiment`, the 200–500 s terrain-field
  leaves). Replace them with the focused field cases below; do not keep a second
  solver alive to justify its tests.
- Keep: `Renderer`, `RenderLane`, `FrameFence`, `GridPass` chunk uploads,
  culling and indirect draws, particle/fluid draw paths, `RetroPass` resolves,
  bloom and present, `TrianglePass`, `mesh_bindless`, the native DDA source
  traversal and material projection primitives.

## Migration

1. Land the stability fixes independently (remove receiver-window gating from the
   frame path, clamp zoom, gate measurement logs). This makes the current app
   usable while the replacement is built.
2. Add `WorldLightSourceGpu` and `WorldLightField` behind an explicit lighting
   mode. Keep the old path selectable during bring-up; do not keep it as a
   production alternative after validation.
3. Add the single fragment shading entry that samples the new field. Compare
   captures at 1024² and 2048² against the current implementation.
4. Flip the default, delete the old pass/shaders/resources and retire the tests
   that only mirror the deleted internals.
5. Add eye adaptation and the SPD bloom; keep AgX.
6. Run the ordinary and GPU-assisted validation sequences, then the performance
   matrix.

Repository strategy: continue in the current checkout. Make a backup commit of
the current dirty tree before touching the lighting implementation, then commit
after each important update (source module, field module, shading, deletion,
post chain, validation). Keep commits atomic and descriptive. Preserve the
unrelated worldgen/asset/UI work; stage the lighting deletion behind a validated
flip rather than deleting the old path first.

### Delivery discipline

- Keep only features that the game uses or that a required gate proves. Delete
  speculative diagnostics, alternate solvers, dormant transports and unused
  scheduler machinery instead of preserving them "for later".
- No new framework, generic scheduler, publication protocol or abstraction unless
  this design requires it.
- Each milestone commits and validates its own slice; do not accumulate another
  large dirty tree.
- When a feature is superseded, remove it in the same series that makes the
  replacement the default.

### Diagnostic identity, captures and latency

The publication/lease/receiver-window identity is gone, but tests and showcases
still need a small observable signal:

- `RenderFrameResult` keeps only `sourceRevision` (last consumed world/Matter/sky
  revision) and an optional monotonic `fieldCycle`/`fieldCursor` for diagnostics.
  These never gate presentation and are not a coverage certificate.
- The game's `m_lighting_revision`/door/lamp checks use `sourceRevision` for
  identity and image/pixel-region comparison for the visible result.
- Edit latency is measured with a test-only, low-cadence readback of a few
  affected probe states (sample count/revision) after a fence, or by an image
  difference in the affected region. No production per-frame readback returns.
- The legacy `LightingScene.Capture` harness is retired or ported to the new
  field; the door/lamp showcase remains as image captures plus source-revision
  receipts. Gameplay illumination queries are removed (they are currently an
  observation log, not a mechanic). A future mechanic gets a direct sample API
  from the field owner.

## Performance and acceptance

- Metrics are measured, not asserted from estimates: per-frame lighting GPU
  time, refresh-cycle length, field memory, source upload bytes, scene frame
  time and total frame time.
- 1024²: field work and refresh cycle must fit the 60 Hz target with spacing 2.
- 2048²: spacing 2, same per-frame ray budget as the prototype, longer cycle.
  The acceptance question is "playable and visually acceptable", not "same
  cycle length as 1024²".
- 4096²: benchmark only, reported with actual cost and memory; failure there does
  not block delivery.
- Camera pan/zoom and window resize must not terminate the application and must
  not emit per-frame lighting logs.
- Edits must become visible within one refresh cycle of the affected probes
  (target: a few hundred milliseconds at 2048²), measured from the committed
  edit to the presented change.
- Memory: report the actual field, source, history and scene allocations. The
  prior 2048² whole-process peak was ~1.37 GiB; the new field should stay within
  the existing device budget and be reported even if it is lower.

## Verification

Keep the CPU oracle tier that already earns its place (`App.Lighting.Tests`:
interval algebra, hierarchical/analytic trace, material/optics), the GridPass
layout/cull/dirty-cache cases, the render-view/camera contract cases, and a
minimal FrameFence/RenderLane lifetime set. Do not keep tests that mirror the
deleted publication machinery.

New focused cases:

- CPU/pure: level/probe/direction indexing and layout, dirty-rect to probe
  invalidation, per-direction accumulation and reset, cascade composite math,
  source revision/upload planning.
- GPU small scene: one emitter against an analytic/CPU falloff; convergence and
  monotonic per-direction sample counts; edit reset; sky change; finite output;
  camera independence (two cameras produce the same field samples for the same
  world position); translucent clipping by terrain.
- Scene/app: emitter on/off/move, door open/closed, Matter tick, pan, zoom,
  resize, shutdown, and no per-frame lighting logs. These are user-visible
  behaviour checks, not publication-state assertions.
- Post: histogram/adaptation target and clamp, fixed-exposure determinism,
  black-frame handling, SPD bloom presence and energy, AgX output monotonicity,
  HDR finite values.
- Captures and visual review at 1024² and 2048²: a dark single-emitter scene, a
  daylight scene, and the door/lamp showcase.
- Performance and memory matrix at 1024/2048/4096 with final binary/shader/
  settings identity recorded, plus the ordinary and GPU-assisted validation
  runs. Do not run the old 200–500 s precision suites or duplicate captures.

## Risks and mitigations

- **2048² spacing 2 is four times the prototype's probe count.** Mitigation:
  fixed per-frame ray budget and longer cycle; if visual convergence is too slow,
  raise `N_max`/retention before coarsening spacing. Do not silently switch to
  spacing 4.
- **Triangle/indirect shading integration with the chunk atlas.** Mitigation:
  keep the existing `GridPass` upload/cull/draw lane and replace only the
  fragment body; do not build a second terrain renderer.
- **Small-feature light leaks at spacing 2.** Accepted for now, per user. Do not
  reintroduce a receiver micro-correction pass to hide it in the first version.
- **Matter projection simplification.** The dense overlay scatter is order
  non-deterministic under float atomics. It is acceptable for the field; tests
  use tolerances. If a future gate needs bit-exact coverage, add a deterministic
  variant then, not a radix sort now.
- **Large deletion touching a dirty tree.** Mitigation: staged migration above;
  preserve unrelated work; retire only code reachable from the replaced lighting
  path, and leave dormant diagnostics until the replacement passes.

## Builder decisions left open

These do not change what is built and can be settled during planning:
exact radiance packing (FP32 first, half later only on measured memory need),
whether the work list is CPU-built or GPU-indirect in the first cut, the exact cascade
level count per map size, the initial `N_max`/retention values, and whether
particles/fluids sample the field in the first cut or in a follow-up.
