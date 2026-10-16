# Canonical world-generation packs

Content uses named, individually described assets in explicitly mounted packs.
`loadContentCatalog()` returns an owned all-World definition catalog. Selecting
one World creates its numeric catalog with explicit bindings and actual compile
parameters. Preparing its texture payloads is a separate asynchronous operation.
Definition validation does not map, hash, decode or generate mips for binary images.

The pack manifest and asset descriptors use schema **1**. The separate standalone
numeric-definition envelope remains schema **2**; the internal flattened content
model is not a pack file format. The World checkpoint envelope is **4**, distinct
from raw sim snapshot **4**. Old seven-document packs and earlier checkpoint
envelopes are rejected rather than adapted. Generator versions remain owned by
the sim implementation. Historical acceptance below predates this migration.

## Ownership and loading

| Owner | Responsibility |
|---|---|
| `engine.asset` | Names, stable typed identity, immutable pack indices, checked importing, shared asynchronous preparation lifecycle. |
| `engine.image` | Texture descriptors, mapping/hash/decode/mip stages and image budgets. |
| `engine.sim` | Element/Material physics, World/WorldPalette/Profile schemas, explicit-binding validation and numeric lowering. |
| `game.colony.content` | World appearance composition, backgrounds, multi-pack catalogs and presentation sidecars. |
| Application and render passes | Activation, GPU upload, fenced retirement, editing and capture policy. |

Asset depends only on core. Sim does not depend on image or rendering. The shared
request is typed by its processor; it is not a universal payload registry or
dependency scheduler. Future domain processors can use the same request lifecycle;
audio and mesh pack processing are not implemented by this delivery.

`ContentSourceSet` owns manifests and per-descriptor bytes. An immutable
`ContentSourceSnapshot` validates a complete candidate and pins one built pack
index. Each descriptor goes through core tree decode/resolve once per import.
The required synchronous `DescriptorReader` supports edited owned bytes through
that same path: a missing record is an error, never a fallback to disk. Callback
views do not escape importing. Source locations and typed values remain owned.

Per-file limits are 64 KiB per manifest and 1 MiB per descriptor. The combined
retained-text limit is `7 * asset::kMaxDescriptorBytes + asset::kMaxManifestBytes`
across all mounted manifests/descriptors. This is a logical source cap, not a
process-peak or history-memory bound. Canonical containment, physical file aliases
and metadata errors are checked; exceptional network alias comparisons can remain
quadratic. Allocation failure reaches the fatal boundary after admitted work drains.

## Manifest, names and multiple packs

A manifest declares every descriptor explicitly. This fragment illustrates local
and dependency references; it is not a complete runnable pack:

```toml
schema_version = 1
pack_id = "ppr.base"

[requires.shared]
pack_id = "ppr.shared"

[assets.rock]
kind = "sim.material"
descriptor = "materials/asset-rock.toml"

[assets.rock_element]
kind = "sim.element"
descriptor = "elements/asset-rock_element.toml"
```

Local keys use lowercase ASCII `[a-z0-9_-]{1,128}`. Namespaces use nonempty,
dot-separated local-key segments, with 128 bytes total. An asset reference is
either a local key or exactly `namespace:key`. Unqualified references resolve
only in the declaring pack; qualified references require a declared dependency.
There is no discovery, implicit override or last-mounted-wins policy. Duplicate
namespaces, keys, hash collisions, missing targets and wrong kinds fail.

The stable identity is pack namespace, local key and declared kind, encoded by a
versioned deterministic hash. Names remain available to reject hash collisions.
Moving a descriptor preserves identity; changing its key or namespace changes it.
Asset keys are separate from numeric Material IDs, catalog slots, generations
and processing revisions. See the [implementation plan](plans/asset-pipeline-plan.md)
for frozen hash encoding and test vectors.

`--worldgen-root=<directory>` mounts one pack. Alternatively,
`--worldgen-pack-set=<file>` mounts an explicit schema-1 root list, relative to
that file; it does not also mount the default pack. Supplying both selectors is
an error. A pack-set example is:

```toml
schema_version = 1
[packs.base]
root = "packs/base"
[packs.shared]
root = "packs/shared"
```

## Element, Material and Texture descriptors

An Element owns phase/density. A Material retains distinct cell identity even
when it shares an Element with another Material. These fragments belong in
separate manifest-indexed files:

```toml
schema_version = 1
phase = "solid"
density = 2200.0
```

```toml
schema_version = 1
element = "rock_element"
representation = "terrain"
diggable = true

[appearance]
display_name = "Rock"
color_rgba = "B99A6CFF"
albedo_texture = "rock_albedo"
normal_texture = "ppr.shared:rock_normal"
texture_period_m = 2.0
```

Appearance references name Texture assets, not raw image paths. A Texture owns
its source path and processing settings:

```toml
schema_version = 1
source = "textures/rock.png"
interpretation = "color"
mip_policy = "mitchell"
full_chain = true
high_quality = false
preserve_coverage = false
alpha_cutoff = 0.5
```

Interpretation is `color`, `data` or `normal`. Normal processing renormalizes
vectors, including level zero, and rejects coverage preservation. Source paths
remain contained in their declaring pack. Albedo/background references require
color interpretation; normal references require normal interpretation. Descriptor
metadata owns property domains; runtime validation and authoring controls reuse
them. Cross-field suitability and numeric safety remain separate checks.

Field/Biome/Feature/Rule descriptors wrap existing sim schemas under `[definition]`.
Game display properties use `[appearance]` where supported. Biome composition,
noise, feature shapes and rule payloads retain their sim-owned interpretation;
splitting files does not create independently compilable numeric biomes or rules.
Marker references remain feature-scoped and rule references remain World-scoped.
See [sim navigation](../lib/engine/sim/codemap.md) for owning schemas and compilers.

## World palettes and identities

A World owns `semantic_key`, `[profile]`, explicit `[palette]` bindings, and
optional appearance/background references. An illustrative palette fragment is:

```toml
[palette.materials.sandstone]
asset = "material_sandstone"
id = 54
```

Other palettes use `asset` and `index` for Fields, Biomes and Features; Rules
name standalone assets under the World palette. Alias references inside numeric
definitions resolve in this World, not in a global asset-name pool. Alias keys
continue to determine semantic RNG streams. Vacuum remains ID zero. Palettes
must provide the exact valid numeric domains and include unsampled biomes whose
limits affect raster work. Mount order and unrelated packs do not assign IDs.

Selected-World lowering uses actual cell size, reference thickness and actor
half-extents. `compileWorldPlan` consumes the retained matching recipe rather
than compiling a second lexical binding set. Draft/save validation lowers every
declared World with request-owned compile parameters; a broken unselected World
rejects the complete candidate without processing textures.
Numeric selection and previews support their explicit parameter domains. The
current prepared runtime/driver supports only the canonical application metric
and actor size; it rejects other recipes before generation/restoration instead
of labeling them with default runtime metadata.

| Identity | Includes |
|---|---|
| Stable asset | Pack namespace, local key and kind. |
| Numeric World | Physical/numeric definitions, semantic aliases, explicit bindings and compile parameters/versions. |
| Presentation descriptor | Selected appearance, texture descriptors and settings. |
| Joined presentation | Numeric World identity, presentation descriptors, binary source-byte identities and processing revisions. |
| Provenance | Source paths, comments and locations for diagnostics/editing. |

Tint or texture changes affect presentation without changing numeric terrain
identity. Physical changes affect numeric compatibility. Comments update
provenance without semantic changes. Absolute roots, mount order and request
generations are not persistent semantic identities. Hashes detect changes; they
are not cryptographic authentication. Mip output bytes need independent checks
when processor algorithms or scheduling change.

## Asynchronous presentation

`asset::PreparationRequest<Processor>` owns bounded admission, polling,
cancellation, ordered outcomes, capture settlement and draining on the existing
scheduler. Each worker returns a fresh `future<Outcome>`; cumulative diagnostics
remain on the owner. A future becoming ready does not by itself establish that
all accepted captures have been destroyed. Fatal allocation errors take priority
and are reported after draining.

Image supplies metadata preflight and map/hash/decode/mip stages. The request
owner uses decoded dimensions for exact retained-output admission. Owning mappings
keep storage handles alive but are not immutable snapshots: source files must
stay stable during admitted work. Replace binary files completely after draining.

A `ContentPresentation` sidecar owns prepared images and matches the selected
catalog, World/numeric identity and expected generation. Asset-backed capture
requires a matching ready sidecar and returns an explicit error otherwise.
Headless numeric validation can omit preparation. Workshop's external update
polls requests before scheduling capture; no background worker waits for image
work queued to the same pool. WorldJob's separate coordinator may finish/drain.
The existing passes retain GPU upload and fenced retirement ownership.

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
Browser metadata comes from the immutable ownership index; descriptor documents
are decoded lazily and reused while their owned bytes are unchanged. Complete
"Used by" results come from the current validated typed draft. Unvalidated
drafts explicitly show that reverse-reference information is unavailable until
validation. Unvalidated descriptors remain selectable, and source-backed
controls keep invalid typed drafts editable.

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
references retain their declared scope. Worlds explicitly own numeric palette
bindings; cloning a descriptor does not silently append it to those palettes. **Delete selected definition** is undoable. Unresolved
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

**Save** validates the complete owned candidate and stages changed descriptor
files while preserving comments, line endings and untouched text. Unexpected
external changes conflict even for untouched draft files; explicit Reload adopts
external content. Conflicts retain the draft. Descriptors commit before manifests,
and obsolete files are removed only after manifest success. An accepted save advances the session revision; it
advances validation to that revision when the saved draft still matches. Edits
made while saving remain dirty. Each document replacement is atomic; the complete
pack is not a single atomic transaction. Partial failures report committed paths,
retain the draft, and allow retry when disk bytes match the baseline or requested
version. Simultaneous external writes during the optimistic commit are unsupported.

**Export Preview** writes material, biome and temperature P6 PPM images,
`preview.json` envelope 2, and the exact validated source snapshot, including
unsaved edits. Sources are grouped under `source/packs/pack-<namespace>/`, with a
schema-1 pack-set listing relative roots. Reports distinguish numeric and joined
presentation identity and record original roots and texture source identities.
Images contain one pixel per visible fine cell, with highest +Y first. Use a
fresh output directory; completion metadata is written last. Binary assets are
not copied: the exported pack-set is not self-contained. Reproduction needs the
original unchanged binary layout or owned source reconstruction against those
original roots. Exact mode also exports `initial-matter.ppm` and production
generation identity/cache metadata.

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
numeric generation. Plain validation imports definitions and
compiles the owned numeric plan without binary texture preparation; it does not
generate a world. The preview command additionally rasterizes all 1024 chunks
using the injected background pool,
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


## Full world checkpoints

The descriptor pipeline uses checkpoint envelope version 4 and rejects earlier
envelopes. Raw sim snapshot version 4 is a separate format.

`game.colony.checkpoint` wraps the unchanged low-level sim snapshot v4 in a
version-4 game envelope (versioned `PPRWGEN1` magic). It stores the exact pack
schema/generator
version, profile, unsigned seed, content fingerprint and 3:1 metric, plus the
sorted numeric-ID/material-key manifest (including reserved vacuum ID 0), biome
sites, feature instances/segments/omissions and finite blob records, start/resource results, initial
matter spans/fill heights and baseline validity, current particle batches and next ID,
generation report, fixed tick
and frequency, and controlled entity slot/generation. Fields are explicit
little-endian integers and IEEE-754 doubles; strings/counts are length-prefixed.
Earlier envelope versions are rejected explicitly; there is no implicit migration.
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
state; it never regenerates the terrain baseline. Presentation assets are
prepared separately against the compatible selected numeric catalog. Tick and
actor selection
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
persistent checksums and are deliberately absent from checkpoint v4; restored
checkpoints display allocation observations as unrecorded rather than zero cost.

With `--worldgen-report`, admission observations also report attempted/skipped
frames, maximum consecutive skips, maximum controls/clock/probe/total duration,
pacer sleep-slice duration/overshoot and control-poll gap. The clock phase
includes pacing and its event pumps; probe includes frame-credit/completion
work. Durations include OS descheduling. They attribute the measured CPU path,
not native input latency or GPU terrain execution. Ordinary runs leave this
instrumentation disabled.

## Bounded interactive acceptance command

The historical results in this section predate the descriptor/async migration
and do not establish its acceptance. Current execution evidence belongs in the
[asset pipeline progress](plans/asset-pipeline-progress.md).

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
