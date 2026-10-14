# Canonical world-generation packs

The authored model belongs to `engine.sim:worldgen.definition`. Game code owns
appearance, background stacks, source documents and assets. `PackDefinition`
combines them; `ContentCatalog` is an immutable runtime projection. Normal startup,
previews, checkpoints and Workshop validation share this model and
`compileRecipe` → `compileGenerationPlan`. The explicit numeric test and
`--profile100` fixtures remain separate opt-in workloads.

The current compatibility boundaries are definition schema **2**, content schema
**3**, and generator algorithm **8**. Old versions are rejected; there is no
compatibility writer, implicit migration or second authored compilation pipeline.
The checkpoint envelope remains **3** and the raw sim snapshot remains **4**.

The consolidation preserves authored physical, noise, geometric and appearance
values while changing their ownership and paths. Material IDs now come only from
`compileDefinition`'s canonical key bindings. Rules derive phase from their active
payload rather than storing a second phase/kind pair. These identity changes
invalidate earlier pack fingerprints, generated goldens and checkpoints. Earlier
v2 performance/preview evidence does not establish v3 acceptance.

## Pack source boundary

`pack.toml` names the complete seven-document set:

```toml
schema_version = 3
pack_id = "ppr_frontier"
[files]
materials = "materials.toml"
fields = "fields.toml"
biomes = "biomes.toml"
features = "features.toml"
worlds = "worlds.toml"
rules = "rules.toml"
appearance = "appearance.toml"
```

Every document requires `schema_version = 3`. `ContentDocumentBinding` maps each
document to its canonical schema root. `worlds.toml` declares profile owners;
`rules.<profile>.<rule>` attaches to `worlds.<profile>.rules.<rule>`. A profile
authored through nested tables in worlds.toml is a declaration. A profile present
only in rules.toml is an error. Duplicate destinations and missing owners fail
before merged resolution, independently of record ordering.

`loadContentPackSource(root, diagnostics, context)` resolves the explicit root and
files canonically, rejects containment escapes and file aliases, checks regular
files and sizes, and returns owned source bytes. Limits are 64 KiB for the manifest
and 1 MiB per document. Cancellation is checked between bounded reads; mapped-size
changes fail. `decodePackDefinition` merges and resolves through generic core tree
schemas, then validates sim references and game appearance. Logical diagnostics
retain source locations. `loadContentCatalog` additionally validates and loads
required textures before publishing an owned candidate. Allocation failure reaches
the application's fatal boundary.

Core tree decoding preserves table presence, including empty groups, and rejects
ambiguous dotted/literal key segments, arrays and dates. Required, defaulted and
optional members have distinct contracts. A failed candidate never mutates an
accepted catalog; invalid Workshop drafts retain their source and last accepted
preview.

## Materials and appearance

Simulation properties live in materials.toml:

```toml
schema_version = 3
[materials.sandstone]
phase = "solid"
representation = "terrain"
diggable = true
density = 2200.0
```

Visual properties live in appearance.toml under the same semantic owner key:

```toml
schema_version = 3
[appearance.materials.sandstone]
display_name = "Sandstone"
color_rgba = "B59C72FF"
albedo_texture = "textures/sandstone.png"
normal_texture = "textures/sandstone_normal.png"
texture_period_m = 2.0
```

Material keys use core's canonical key grammar. Vacuum is the built-in ID zero;
authored `numeric_id` is unknown and rejected. Terrain materials are solid and
diggable metadata is terrain-only. Density is finite and positive. Particle
materials can be solid, liquid or gas; matter-fill references accept all three
particle phases. Appearance owners must exist in the corresponding
simulation collection. Display names, colors and texture/PBR controls never enter
the sim-owned descriptor model.

These values are game tuning examples, not measured material data. The fragments
reference illustrative paths and do not constitute a complete deployed pack.

Optional appearance fields use these finite domains and defaults:

| Field | Domain | Default |
|---|---|---|
| `metallic`, `roughness` | [0,1] | 0, 1 |
| `emissive_r`, `emissive_g`, `emissive_b` | [0,1] linear RGB | 0 |
| `emissive_strength` | [0, float max] | 1 |
| `alpha_mode` | `opaque`, `blend` | `opaque` |
| `texture_sampling` | `stochastic`, `repeat` | `stochastic` |
| `ior` | [1,3] | 1.5 |
| `normal_texture` | Portable relative path | absent |
| `normal_scale` | [0,4] | 1 |
| `refraction_depth_m` | [0,1] metres | 0 |

A normal or albedo map requires `texture_period_m`. Albedo and background maps use
sRGB decoding; normal maps use linear data decoding and renormalized signed-vector
mips including level zero. The material tint alpha is authored optical opacity;
geometric contour and particle fill coverage are separate. Within covered surface,
`S = opacity*material + (1-opacity)*refracted_background`; coverage composites S
with the destination exactly once. IOR1, flat normals, zero depth and opaque
materials have no displacement. A covered zero-opacity surface may still refract.

Natural materials use three translated patches on a triangular lattice to reduce
repetition. Albedo and normal maps share patch coordinates, weights and explicit
world-space sampling gradients. Linear RGB receives approximate mean/variance
contrast correction; alpha uses ordinary blending. This is not full histogram
transport. `texture_sampling = "repeat"` preserves structured patterns such as
masonry mortar, using one matching albedo/normal lookup. Both modes remain anchored
in world coordinates, including particles. Sampling mode participates in content
identity; changing it intentionally invalidates checkpoints tied to the old pack.

Solid boundary strokes retain their opacity independently of material optical
opacity. Translucent solids participate in internal solid interfaces; gas and
liquid particles leave the terrain boundary exterior.

Texture paths use portable relative slash-separated components. Absolute,
drive-qualified, backslash, empty, dot, and parent components are rejected.
Typed pack resolution performs no filesystem access. The complete pack loader validates existence, canonical containment, required image decoding, byte budgets and source-byte identity.

## Biomes and noise

Fields use `NoiseFieldDesc::describe()` directly. Each field requires an algorithm
and a finite positive frequency. Supported algorithms are `open_simplex2`,
`perlin`, `cellular_distance` and `cellular_value`; fractals, warp and work bounds
come from that same schema. Compiled noise is immutable and allocation-free when
sampling. No parser or renderer dependency enters sim.

Biome properties are nested canonical groups, for example:

```toml
schema_version = 3
[biomes.temperate.temperature]
field = "temperature"
base_k = 295.0
variation_k = 10.0
[biomes.temperate.composition.host]
order = 10
field = "composition"
[biomes.temperate.composition.host.depth]
min = 0.0
max = 1.0
[biomes.temperate.composition.host.materials.stone]
material = "sandstone"
weight = 1.0
```

Appearance provides `appearance.biomes.<key>.display_name` and
`preview_color_rgba`. Biomes support ordered composition/deposits and optional
`pore`, `lining`, `surface_layers.soil`, `surface_layers.weathered`, `formation`
and `rind` groups. `relaxation`, `sprinkle`, `finish` and `erosion` are defaulted
groups. Deposits use `depth` and `threshold` intervals; presence of `blob` selects
blob geometry instead of an authored shape discriminator. Blob radius, stretch
and orientation are nested groups. There is no duplicated threshold kind.

The supported scope is 64 biomes, 16 composition bands per biome, 64 choices per
band and 64 deposits per biome. Ordered composition covers normalized depth [0,1]
without gaps or overlaps. Choice weights are positive and accumulated in canonical
key order; overflow or collapsed normalized intervals fail. Terrain choices may
reference `vacuum`; deposits require suitable terrain materials. Temperature base
must exceed its nonnegative variation. Halo/work limits remain checked by sim;
do not infer a new engine-wide budget from these content limits.

## Features and rules

Features own `reservation`, required `allow_horizontal_reflection`, keyed
`operations` and keyed `markers`. Each operation has order, x/y coordinates and a
tagged action. Paint owns a material reference; carve has none. Both own a tagged
rectangle, ellipse or capsule shape. A rectangle paint example is:

```toml
[features.landing.operations.floor]
order = 10
x_fine = 0
y_fine = 0
[features.landing.operations.floor.action]
tag = "paint"
[features.landing.operations.floor.action.paint]
material = "sandstone"
[features.landing.operations.floor.action.paint.shape]
tag = "rectangle"
[features.landing.operations.floor.action.paint.shape.rectangle]
width_fine = 30
height_fine = 3
```

Marker roles are `start`, `traversal_room`, `sealed_pocket`, `resource_anchor` and
`inert`. Marker references resolve in the selected feature, never in a global
same-name pool. Bounds, role suitability, canonical fine lengths and reservation
containment are validated. Appearance supplies feature display names.

Rules are authored separately, scoped by their profile:

```toml
schema_version = 3
[rules.frontier.caves]
order = 10
enabled = true
[rules.frontier.caves.payload]
tag = "cave"
[rules.frontier.caves.payload.cave]
biome = "temperate"
field = "caves"
[rules.frontier.caves.payload.cave.depth]
min = 0.0
max = 1.0
[rules.frontier.caves.payload.cave.threshold]
min = 0.25
max = 1.0
```

The payload alternatives are `cave`, `carve_worm`, `vein_worm`, `connection`,
`feature` and `matter_fill`. Worms own a `geometry` group; vein worms additionally
own material/host references. Phase is derived from the payload. Disabled rules
retain authored identity but produce no numeric work. Matter fill references an
enabled feature placement in the same profile and a suitable sealed marker in
that placement's feature. Solid/liquid fills require height; gas fills require
height to be absent. Unsupported tags and inactive payload fields are errors.

## Profiles, identity and runtime

Profiles own `layout`, `boundaries`, optional `surface_bedrock`, `start`, keyed
`resources`, keyed `biomes`, and attached `rules`. Their display name and
background-stack choice live in `appearance.worlds.<profile>`. Background stacks
live in `appearance.backgrounds.<stack>.layers.<layer>` with order, texture,
color, repeat period and parallax factor. Four layers have unique orders zero
through three; the farthest tint is opaque.

The metric remains three fine cells per metre and one metre reference thickness.
Cell centres are `(i + 0.5) / 3`; picking uses half-open intervals. Discrete authored
lengths must equal canonical fine boundaries. Construction tiles remain aligned
globally, including across chunk seams. The numeric compile parameters provide
the actual actor half-extents .4/.9 m; profiles cannot resize actors.

`SemanticFingerprint` is shared by definition, rules, materials and full-pack
identity. It walks described members in declaration order and keyed collections
in sorted key order. Optional presence, textual variant tags and active payloads
are explicit. References encode collection, scope, suitability, key and actual
owner path. Numeric values use little-endian bytes, lengths are explicit, and
negative zero is normalized. Indices, assigned IDs, comments, source ordering and
provenance never define semantic identity.

Definition identity uses `ppr.worldgen.definition.v2`. The rules projection uses
`ppr.worldgen.rules.v2`, framing profile keys in canonical order, including empty
profiles. Material identity uses `ppr.materials.v3`. Full-pack identity uses
`ppr.content.v3`, schema version, pack ID, complete definition, appearance and
canonically ordered asset-usage identities (path, normal/albedo usage, byte count
and source-byte checksum). FNV-1a identities detect content changes; they are not
cryptographic authentication. Shipped v3 goldens require explicit validation.
Appearance colors remain described strings: changing hexadecimal letter case
conservatively changes identity even when the rendered color is identical.

The game retains renderer-independent simulation ownership and injected execution
dependencies. A preparation candidate owns its catalog and complete numeric world.
Cancellation drains accepted work before borrowed input release. The coordinator
keeps gameplay paused while control/render work continues, and publishes only
after matching GPU activation/presentation. Teardown reverses setup; allocation
failure remains fatal. See the diagnostic and checkpoint sections below for the
existing bounded workload and evidence contracts.

## In-game Worldgen Workshop

Open **Worldgen Workshop** from the colony panel, or start directly without
generating a complete world:

```text
app.game --worldgen-workshop --worldgen-root=E:/Code/ppr/assets/worldgen/default
```

The pack-root field defaults to the current world-generation root, including any
`--worldgen-root` override. The executable-adjacent `worldgen/default` directory is
staged output and may be replaced by a build; choose the authored pack root when
edits should persist across builds.

The browser shows the opened pack, materials/elements, fields, biomes, features,
world profiles, profile-owned rules, appearance and textures. Schema descriptors
provide the nested groups, tagged payloads, ranges and reference choices. Rules
retain their world-profile ownership across document boundaries. Pack manifests and referenced textures remain inspectable.

The resizable Workshop separates the content browser from the inspector. Search
definitions in the browser or filter the inspector's nested property tree.
Numeric sliders use the production decoder's legal ranges, with practical editing
ranges for unbounded values; text entry remains available for precise values.
Enums and content references offer their available choices in combo boxes.
Optional references can be unset through an undoable source edit.
Clearing an optional property removes its source subtree; Undo restores its
exact bytes. Changing a variant tag replaces its tag and active payload in one
reversible transaction. Required payload values may need editing before validation
succeeds; invalid drafts remain editable.
Property errors retain their source location and appear beside the relevant
control. Select a diagnostic to reveal that property, including nested groups.
Linked constraints such as ordered radii still apply alongside individual ranges.
Live preview pauses on a known invalid draft until an edit fixes it, retaining the
last successful picture. Open and Save explain when their prerequisites are missing.

**Create from selected** clones a definition with a new semantic key. Scoped
rules and appearance entries retain their owner. Numeric IDs are derived during
compilation and are never authored or selected in the editor. **Delete selected definition** is undoable. Unresolved
references remain visible as validation errors and block saving or generation;
deletion never silently cascades. Structural editing supports the explicit-table
and dotted-record syntax used by the shipped packs; inline-table record deletion
returns an explicit unsupported-operation error. Ordinary property editing retains semantic keys; cloning changes identity.

Use a fixed seed, profile and sample origin, then **Preview** to compare edits.
**Automatic live preview** regenerates after a 350 ms quiet period. It keeps one
running request and one replacement, coalesces edits, and defers generation while
explicit open/save/export operations run. Cancel suppresses automatic retry until
the desired input changes or live preview is toggled again.

Choose **Biome sample** for the fast local laboratory or **Whole-world crop** for
production terrain at the requested coordinates.
The laboratory generates a 256-by-256 fine-cell biome sample, approximately
85-by-85 metres, with at most 25 resident support chunks. It reuses production
geology, pores, worms, material filters, appended erosion and supported formation
evaluation. It omits global start areas, authored feature reservations,
connections, initial matter and whole-world playability validation. This is a
**Biome sample**, not an exact crop of a complete generated world.

**Whole-world crop** first generates and validates the complete production world
in the background, preserving global formation admission/order, authored features,
start constraints and initial matter. It retains an immutable world cache and
copies only the crop's support chunks for presentation. Moving the crop reuses the
cache; changing content, profile or seed requires another complete preparation.
The selected biome is ignored in this mode. Its first preview costs a full world
generation and retains the full terrain and initial particle snapshot in memory.
The background coordinator injects a separate existing worker pool for production
chunk generation; crop extraction itself performs no additional terrain generation.

The cyan outline marks the sample interior. Complete support chunks remain
visible outside it for contours and formations; counts and exports refer only to
the interior. Textured, material, biome and temperature views use the real
GridPass. Temperature display and export use 200–1000 K.

Edits have Undo/Redo and Revert. Invalid drafts retain the last validated catalog
and successful preview. Preview and I/O run on owned background snapshots; Cancel
and newer requests cannot publish an obsolete result. Open, Reload and application
exit require an explicit decision before discarding dirty drafts.

**Save** validates against the current rest of the pack and stages every changed
content document while preserving comments, line endings and untouched text. Disk
conflicts retain the draft. An accepted save advances the session revision; it
advances validation to that revision when the saved draft still matches. Edits
made while saving remain dirty. Each document replacement is atomic; the complete
pack is not a single atomic transaction. Partial failures report committed paths,
retain the draft, and allow retry when disk bytes match the baseline or requested
version. Simultaneous external writes during the optimistic commit are unsupported.

**Export Preview** writes material, biome and temperature P6 PPM images,
`preview.json`, and the exact validated manifest/source snapshot used by the
completed preview, including unsaved edits. Images contain one pixel per visible
fine cell, with highest +Y first. Use a fresh output directory; completion metadata
is written last. Referenced texture identities are recorded, but assets are not
copied: reproduction requires the original unchanged asset root. Exact mode also
exports `initial-matter.ppm` and production generation identity/cache metadata.

Returning from Workshop restores the retained game presentation, camera and
pause state without regenerating its terrain. Shared `engine.core` command history
owns bounded reversible edits; content-specific patches and disk dirty state stay
in the game authoring session.

## Command-line diagnostics

Build `app.game` or `game.tests` to synchronize the build-owned `worldgen` tree,
including deleted assets, without needing a C++ relink. Then run from any folder:

```text
app.game --validate-worldgen
app.game --worldgen-preview --worldgen-profile=hollows --worldgen-output=<directory>
app.game --validate-worldgen --worldgen-root=<explicit-pack-directory>
```

The default root is `worldgen/default` beside the executable. This command reads
and decodes the complete pack before reporting its fingerprint; it creates no
window, RHI service or GPU work. `--worldgen-profile` selects a validated profile;
`--worldgen-seed` accepts the complete unsigned 64-bit range for subsequent
numeric generation. Plain validation loads and validates content and assets and
compiles the owned numeric plan; it does not generate a world. The preview command
additionally rasterizes all 1024 chunks using the injected background pool,
resolves initial matter, and validates final terrain and the supported start.
Its report contains canonical terrain/output checksums, five phase
timings, metadata counts and resident grid payload bytes. Checksums use FNV-1a
over explicit little-endian integers and floating bit patterns; terrain is
chunk-major/local row-major. Output identity includes stable biome/feature/rule
keys, geometry, omissions, start/resource results and matter records/spans.
Timings and execution counters do not participate. Payload bytes exclude
metadata, allocator overhead, scratch and the active predecessor, so they are
not a peak-memory measurement.
Diagnostic output is limited to 64 entries and bounded
field lengths. The asynchronous runtime job uses the same loader.

Stage 3 currently provides owned numeric plans, deterministic macro layout,
bounded worm/blob/room/feature geometry, chunk-local terrain rasterization and an
owned full-grid executor with injected scheduling and cancellation. Every
accepted chunk task drains before borrowed inputs are released. Sealed-pocket
markers override traversal-room markers; room connections cannot open those
reservations. Allocation exceptions remain fatal, including during task draining.

Initial matter resolution now produces disjoint canonical spans, contour-clipped
volume and mass, with gas filling remaining volume. Start validation checks the
actual actor footprint, supported level routes and nearby exposed resource
components. Components, aggregate searches, spans and regions are capped at
1,048,576 cells, 4,194,304 cells, 1,048,576 spans and 4096 regions respectively;
start contour validation permits at most 262,144 dual cells.

Complete numeric results own the plan, layout, geometry, grid, start, matter
and report; the injected clock affects timings only. Game factories bind that
result to an owned catalog and immutable reproduction descriptor; Colony can
adopt the complete owner without regenerating. The production job builds and publishes this complete candidate. Full allocation/scratch accounting remains an acceptance measurement.
Third-party dependency cleanup is deferred until all seven implementation stages
are complete, as requested.


## Full world checkpoints

`game.colony.checkpoint` wraps the unchanged low-level sim snapshot v4 in a
version-3 game envelope (versioned `PPRWGEN1` magic). It stores the exact pack schema/generator
version, profile, unsigned seed, content fingerprint and 3:1 metric, plus the
sorted numeric-ID/material-key manifest (including reserved vacuum ID 0), biome
sites, feature instances/segments/omissions and finite blob records, start/resource results, initial
matter spans/fill heights and baseline validity, current particle batches and next ID,
generation report, fixed tick
and frequency, and controlled entity slot/generation. Fields are explicit
little-endian integers and IEEE-754 doubles; strings/counts are length-prefixed.
Version-1 and version-2 envelopes are rejected explicitly; there is no implicit migration.
The complete encoded envelope is bounded at 256 MiB. This is an application
checkpoint for the current component layouts, not an ABI migration facility.

`WorldJob::requestCheckpoint` transfers the frozen driver at a completed tick
boundary to the existing single coordinator. The owner must pause ticks/tools,
mirror body state and defer all live access and acknowledgments while leased.
The worker captures every resident chunk, including clean chunks, without
changing dirty rectangles. Durable ECS/body state and awake flags are retained;
Plan, PathReq and PathComp rows are discarded because they are derived work.
The result owns its data. After preparation, move it with `takeCheckpoint`,
return the identical session with `takeCaptured`, restore the prior pause state
and clear held input. Encoding and file I/O must run off-main from the owned
result. No production save UI, save slots or autosave are provided.

Decoding rejects unsupported identity/metric, malformed/truncated/trailing
bytes, excessive counts, unknown or particle terrain IDs, bad entity selection,
invalid component layouts, unordered/overlapping matter spans and nonfinite
metadata before constructing a candidate. Restore creates a fresh terrain grid
from saved cells and reconstructs derived biome lookup, physics and presentation
state; it never regenerates the terrain baseline. Tick and actor selection
survive; held input and GPU descriptors do not. Body values survive, but Box2D
solver/contact internals are rebuilt rather than promising bit-exact solver
continuation. An edited world remains loadable when historical generation containment or the
starting route has changed. The baseline-valid flag describes historical generation
metadata only and does not hide or delete current matter. Current particle
records restore directly without re-seeding, with unique IDs and owning fine cells,
finite positive physical values, coherent mass/density and nominal cell-volume bounds.
The limit is 1,048,576 logical records (64-byte CPU, 59-byte wire encoding); immutable
snapshots can retain older batches. This is a logical payload cap rather than a
process-memory ceiling.


## Headless diagnostic exports

After building `app.game`, run:

```text
app.game --worldgen-preview --worldgen-profile=frontier --worldgen-seed=42 --worldgen-output=<output-directory>
```

The headless command generates one exclusively owned world using the existing
background pool, validates it, then streams four flat P6 PPM images:
`material.ppm`, `biome.ppm`, `temperature.ppm`, and `initial-matter.ppm`. It also
writes `report.json` (at most 1 MiB) and `reproduce.txt`. Use a separate output
directory for each profile/seed. Existing named outputs are overwritten; an I/O
error or cancellation is reported and may leave partial files. A successful
report applies to that completed invocation only.

Each image is 1024 by 1024 RGB, with +Y world top in the first row. A pixel
averages all 16 fine samples in its 4 by 4 footprint rather than dropping thin
features through point sampling. Material colors and biome colors come from the
catalog; vacuum is black in material and temperature maps. The temperature ramp
clamps 200..1000 K from blue through green to red. Biomes cover nominal empty
cells too. Initial matter is the unsimulated generation baseline: colors are
weighted by the shared contour's empty polygon area and clipped at the region's
fill height. Black means no initial matter coverage. There is no simulated
pressure, flow, equilibrium or live oxygen field in this image.

The JSON report records descriptor/metric identity, hexadecimal seed/checksums,
phase seconds, resident payload bytes, metadata counts, material counts, authored
biome color legend and each initial region's material, density, volume, mass,
fill height and span count. The bounded streaming writer retains rows rather
than all full-resolution image buffers. These overviews help author geometry
and distributions; they do not show albedo texturing or certify GPU contour,
seam, traversal or frame-cadence acceptance. Those require separate GPU-rendered
closeups and runtime checks.

For actual textured GridPass closeups, explicitly opt into GPU initialization:

```text
app.game --worldgen-preview --worldgen-profile=frontier --worldgen-seed=42 --worldgen-gpu-output=<closeup-directory>
```

This separate offscreen diagnostic freezes the generated session, renders
512-square start, chunk-seam, surface and texture views, and reads back RGB PPM
images. `gpu-cameras.txt` records descriptor identity and exact metre cameras.
It uses the real bindless terrain pass and blocks for GPU completion only in
this diagnostic host. Plain --validate-worldgen loads and compiles the pack without generating terrain, opening a window or initializing the GPU. The distinct --worldgen-preview selector generates diagnostics and requires at least one explicit output directory. The earlier --validate-worldgen --worldgen-output form remains a CPU-only compatibility path.
These images do not measure interactive loading or input responsiveness.

## Interactive cadence observation

`--worldgen-report` prints bounded, allocation-free frame-path observations at
shutdown. The main update timer includes world-job/publication polling; control
servicing during frame admission is timed separately. Per observed phase it
records update count, maximum update gap/work duration and successful presented
acknowledgment count/gap. Startup-to-first-loading and loading-to-first-complete
world acknowledgment are separate. Observations aggregate all jobs in the run;
the printed seed is the last requested seed. Missing readiness milestones are
not reported as zero-duration success.

The clock is injected at the application boundary. Presentation observations
are CPU completion-consumption times, not GPU timestamps, precise scanout times,
or native input/Cancel latency. Background outstanding tasks include executing
work and are not queue depth or worker utilization. No GPU-thread-confined pass
counters are read concurrently by the interactive report.

The offscreen GPU manifest additionally records CPU submission duration, host
completion wait and existing atlas/resident-table counters. Host completion wait
is not isolated GPU terrain cost. The diagnostic host is frozen and blocking;
its numbers do not certify interactive responsiveness.

Content diagnostics retain the first 64 records in production loading jobs and
CLI commands. Nested schema collectors preserve the total omitted count when
forwarding that prefix. The UI also bounds displayed text; source/texture byte
limits are independent. Allocation failures still escape to the fatal application
boundary after accepted worker tasks drain.

## Generation memory observations

Newly generated reports include `memory_accounting_available`,
`retained_known_payload_bytes` and five `known_phase_scratch_bytes` values in
layout/features/raster/matter/validation order. The world-generation panel shows
these payload observations in MiB. Retained values cover the numeric recipe,
spatial layout, resident grid, geometry, initial-matter spans, start-resource
counts and material-count arrays, using owned vector capacities. Scratch values
cover overlapping known temporary payloads; raster chunk buffers use a
conservative live-task requested-payload bound after admitted tasks drain.

These figures exclude allocator metadata, simultaneous old/new reallocation
buffers, opaque noise and task allocations, thread stacks, content catalogs,
physics and GPU storage. They are neither total allocations nor process peak
memory. Record process peaks separately. These observations do not affect
persistent checksums and are deliberately absent from checkpoint v3; restored
checkpoints display allocation observations as unrecorded rather than zero cost.

With `--worldgen-report`, admission observations also report attempted/skipped
frames, maximum consecutive skips, maximum controls/clock/probe/total duration,
pacer sleep-slice duration/overshoot and control-poll gap. The clock phase
includes pacing and its event pumps; probe includes frame-credit/completion
work. Durations include OS descheduling. They attribute the measured CPU path,
not native input latency or GPU terrain execution. Ordinary runs leave this
instrumentation disabled.

## Bounded interactive acceptance command

Run `app.game --worldgen-acceptance --worldgen-profile=frontier --worldgen-seed=42`
(and repeat with `hollows`) on the desired backend. It enables the cadence report,
waits for current-world presentation and warms the completion-selected slots,
then measures stationary and moving/zooming cameras, a real two-cell chunk-seam
dig through the publication transaction, biome/textured preview switches, and two
New Games (seed 1, then the requested seed). It waits for final retirement and
exits. An optional `--exit-after=300` bounds the process; early close or timeout
is failure, never an acceptance pass. Acceptance excludes `--profile100`.

The scenario checks that camera changes upload no terrain or resident tables,
that actual terrain draw/compute commands were encoded, that a successful frame
presents the settled seam edit, and that retained atlas/texture counts return to
the initial settled baseline after reloads. Fixed-size render-owner snapshots
cross the existing frame-result transport; main never reads live pass counters.
Printed deltas count encoded commands, not GPU completion. Texture slots include
per-world high-water cache slots; observed resource peaks sample acknowledged
frames and can miss a transient between them. They are not driver memory or
precise total VRAM measurements.

This scripted production-path exercise does not prove native keyboard/mouse or
Cancel latency, Inspector interaction, actor traversal, scanout cadence or GPU
terrain time. Those require separate interaction/capture evidence. A scenario
PASS covers only its explicit checks; the complete milestone acceptance gates
remain those in the implementation prompt.

Before the final retirement-identity repair, all 12 canonical Dev CTest entries, required
same-process three-loop affected app/asset/checkpoint checks and the Dev
GPU-validation prepared-session smoke passed. The game-suite CTest timeout was
corrected from 300 to 600 seconds for its measured workload. The original
Frontier/42 Release run failed its 300-second bound in textured-preview capture
phase 5. The independently reviewed repair distinguishes resource lifetimes by
job and generation. The same Release commands then passed Frontier/42 in
38.8196 seconds and Hollows/42 in 37.5921 seconds, completing phases 1–8 and
settling to one atlas and 98 texture entries/slots. All 88 affected Dev app
leaves passed again after the repair. Interactive GPU timestamps were unavailable
and native input latency remains unmeasured. Post-repair resource checks passed
three same-process loops on both D3D12 and Vulkan; all six Live targets built and
focused Live app, optical/resource/mip and checkpoint checks passed. Final
format and blocking semantic-style gates passed. CLion inspection was unavailable
and remains unrun. The
[implementation plan](worldgen-overhaul-plan.md#data-quality-iteration) retains
identities, timings, earlier failures and exact evidence paths. The completed GPU
diagnostic captures retain their narrower measurement scope.

The progress modal shows one bar for the current stage. Terrain composition uses completed generation chunks; candidate graphics staging uses acknowledged staged chunks. Physics preparation, diagnostic capture, GPU readiness, publication, cancellation and cleanup have no measured completion ratio and display an animated indeterminate bar. A full staging count is not a promise that the new world has presented.

Terrain materials may author `roundness` in `[0, 1]` (default `1`). Zero keeps
straight marching-square cuts; one uses the original rounded quadratic boundary.
The value is part of canonical content identity and is rejected for particles.
Masonry uses a smaller value than soft clay and sandstone. Global, mip-filtered
noise adds bounded irregularity without moving contour crossings between cells.
The performance widget includes asynchronous GPU command-frame timing alongside
CPU frame cadence and render-lane wall time; GPU samples exclude presentation.
