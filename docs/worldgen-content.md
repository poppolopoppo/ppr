# World-generation content records

The shared numeric seam is `game.colony.metric`: three fine cells per metre,
three by three samples per construction tile, one metre reference thickness.
Cell centres are `(i + 0.5) / 3`; picking uses half-open cell intervals with
canonical `i / 3` boundaries. Discrete authored lengths must equal a canonical
boundary exactly; adjacent floating values are rejected. Construction footprints
use global tile coordinates, can cross 128-cell chunks, and reject the incomplete
final row/column. `WorldMetric` rejects legacy scale/thickness values.

The immutable `engine.math` contour table describes all sixteen centre-sampled
dual-cell cases, midpoint crossings, CCW filled triangles and solid-left boundary
segments. Diagonal solid components remain disconnected. Game, physics and GPU adapters consume the same table for the current production world.

The first implementation slice provides `game.colony.content` and its pure
`decodeMaterialCatalog(bytes, origin, diagnostics)` boundary. It returns an
owned immutable candidate. A failed decode does not modify an earlier catalog.
`std::bad_alloc` escapes to the executable's fatal exception boundary rather
than becoming a recoverable catalog error.
Normal interactive startup and New Game use the shipped authored pack through the asynchronous world coordinator. Explicit numeric many-agent fixtures remain available for tests and profiling.

The decoder reuses `engine.core`'s TOML scalar codec and typed schema resolver.
Its opt-in record mode rejects empty tables and literal dotted/empty key
segments: ordinary flattening would otherwise silently discard empty records
or confuse quoted dots with table separators. Existing scalar configuration
consumers keep their previous decoding behavior. Arrays and dates remain
unsupported.

```toml
schema_version = 1

[materials.sandstone]
numeric_id = 16
display_name = "Sandstone"
phase = "solid"
representation = "terrain"
color_rgba = "B59C72FF"
albedo_texture = "textures/sandstone.png"
texture_period_m = 2.0
diggable = true
reference_density_kg_m3 = 2200.0

[materials.snow]
numeric_id = 1024
display_name = "Snow"
phase = "solid"
representation = "particle"
color_rgba = "FFFFFFFF"
reference_density_kg_m3 = 300.0
```

These values are PPR tuning examples, not measured Away Team values or certified
material data. This fragment references an illustrative texture path; it is
not a deployed pack.

Record names match `[a-z][a-z0-9_]*`. IDs are explicit and unique in 1..65534;
zero and 65535 are reserved. All common fields in the example are required.
Terrain requires solid phase and the three terrain-only fields
`albedo_texture`, `texture_period_m`, and `diggable`. Particle records reject
those fields. Phase accepts solid/liquid/gas; representation accepts
terrain/particle. Density and texture period must be finite and positive.
Colors are exactly eight hexadecimal RRGGBBAA digits authored in sRGB; future
shader presentation must convert RGB to linear space.

Texture paths use portable relative slash-separated components. Absolute,
drive-qualified, backslash, empty, dot, and parent components are rejected.
The material-only decoder performs no filesystem access. The complete pack loader validates existence, canonical containment, required image decoding, byte budgets and source-byte identity.

The current implementation safety caps are 1 MiB of source and 256 material
records. Exceeding a cap fails explicitly. Unknown fields, missing fields,
duplicate IDs, unsupported versions, and invalid values produce source/key
diagnostics. Catalog records are sorted by key; field validation follows a
fixed schema order, independent of TOML record/field ordering.

`materialFingerprint()` is a versioned FNV-1a checksum over canonical resolved
material values, not a cryptographic digest or complete pack fingerprint. It
excludes comments, source paths, and provenance. Byte order is:

1. Length-prefixed UTF-8 `ppr.materials.v1`, then the material count as u64.
2. For each record in key order: length-prefixed key, u16 ID, length-prefixed
   display name, u8 phase (solid=0, liquid=1, gas=2), u8 representation
   (terrain=0, particle=1), u32 RRGGBBAA, and IEEE-754 binary64 density bits.
3. For terrain only: length-prefixed texture path, binary64 period bits,
   and one diggability byte (0 or 1).

All integers and floating bit patterns use little-endian bytes; string lengths
are u64 byte counts. FNV-1a starts at 14695981039346656037, multiplies by
1099511628211 after XORing each byte, and wraps modulo 2^64. The example above
has checksum `6e172b31ab5d1b68`. Textures, biome/profile/template data, generator
version, and world-unit identity must enter the eventual complete pack/world
descriptor separately.

The new behavior is exercised in the existing `GameConfigUnitTests` CTest entry
under `game/content/`; opt-in codec compatibility tests are in
`core/config_file/`. Follow the build-system skill for the required Windows
toolchain shell before configuring, building, or testing.

## Bootstrap integration

The interactive new-game flow keeps gameplay paused while UI/render/control work continues. Its isolated driver candidate uses a persistent low-priority coordinator and a distinct injected chunk pool. Cancellation reaches generation, actor spawning and bounded collider units. Publication swaps owning driver pointers; cancellation/failure retains the previous session. Large disposal runs on the coordinator. The first generation job waits for a successful loading-screen presentation, and the replacement modal waits for a matching presented world frame.

MSVC 14.51.36231 repeatedly crashed with C1001 in function-signature.cpp when the application consumed the independently exported world-job class. The coordinator is now a partition of game.application, and main is thin. This keeps ownership explicit without custom deleters or duplicate facilities. Independent render controls admit a candidate header and at most eight chunks per admitted update. Completion-governed versioned atlas slots replace interactive global-idle writes. Only matching activation publishes the CPU session, and the modal stays until a matching replacement frame presents. End-to-end responsiveness remains an acceptance gate; measured long update/presentation gaps are recorded in the implementation progress report.

## Pack source boundary

`decodePackManifest` uses the existing scalar codec and typed resolution. Every entry is required:

```toml
schema_version = 1
pack_id = "default"
[files]
materials = "materials.toml"
biomes = "biomes.toml"
fields = "fields.toml"
features = "features.toml"
worlds = "worlds.toml"
```

`loadContentPackSource(root, diagnostics, context)` performs blocking I/O and belongs on the world coordinator. It resolves the explicit root and each file canonically, rejects escape paths and file aliases (including aliases of the manifest), checks regular files and byte limits before mapping, and returns owned bytes. Limits are 64 KiB for the manifest and 1 MiB for each of its five documents. Cancellation is checked between bounded file reads; mapped size changes are rejected. Original manifest file-entry locations remain available for load diagnostics. Allocation failures propagate to the fatal application boundary.

This lower boundary returns source bytes. `loadContentCatalog` compiles every document and required texture into one owned candidate used by the interactive authored-world path.

## Biome recipes

`decodeBiomeDefinitions` returns owned recipes sorted by biome key. It validates
references against the decoded material catalog and field definitions. Terrain
choices and deposit hosts/outputs must resolve to terrain solids; particle IDs
are rejected. The document is limited to 1 MiB and 64 biomes. Each biome supports
at most 16 composition bands, 64 named choices per band and 64 deposits.

All fields shown below are required. Base temperature must exceed its nonnegative
variation. The preview color is an eight-digit sRGB RRGGBBAA string. Composition
bands have unique explicit orders and must cover normalized depth `[0,1]`
contiguously in that order. The generator uses half-open bands, with depth one
belonging to the final band. Positive finite weights are accumulated in choice-key
order; overflow and unrepresentable normalized intervals fail validation.

```toml
schema_version = 1
[biomes.temperate]
display_name = "Temperate"
preview_color_rgba = "709040FF"
temperature_base_k = 295.0
temperature_variation_k = 10.0
temperature_field = "temperature"

[biomes.temperate.composition.host]
order = 10
depth_min_normalized = 0.0
depth_max_normalized = 1.0
field = "composition"
[biomes.temperate.composition.host.materials.rock]
material = "sandstone"
weight = 3.0
[biomes.temperate.composition.host.materials.ore]
material = "copper"
weight = 1.0

[biomes.temperate.deposits.copper]
kind = "threshold"
order = 20
field = "ore_lenses"
material = "copper"
host_material = "sandstone"
depth_min_normalized = 0.2
depth_max_normalized = 0.8
threshold_min = 0.3
threshold_max = 1.0
```

Deposits are optional. Each threshold deposit has a unique explicit order,
increasing normalized-depth bounds within `[0,1]`, and increasing threshold
bounds within `[-1,1]`. `threshold` is the supported biome deposit kind at this
boundary; unsupported kinds fail explicitly. Geometric veins, feature templates,
world rules and full-pack identity are validated by the complete pack loader. The
production generator compiles these decoded recipes into its owned numeric plan.

## Noise fields

`decodeFieldDefinitions` resolves named, owned numeric descriptors from `fields.toml`, sorted by stable key. The document requires `schema_version = 1` and at least one `[fields.<name>]` record. The byte cap is 1 MiB and the record cap is 256. Required fields are `algorithm` and positive finite `frequency_per_metre`. Supported algorithms are `open_simplex2`, `perlin`, and `cellular_distance` (Euclidean squared distance).

Optional controls have these documented defaults: `fractal = "none"` (`none`, `fbm`, `ridged`), `octaves = 4` (1..16), `lacunarity = 2.0` (1..4), `gain = 0.5` (0..1), `warp_amplitude_m = 0.0` (0..4096), and `warp_frequency_per_metre = 0.006` (positive finite). Nonzero warp uses an independently seeded OpenSimplex2 progressive warp. Noise samples use global metre coordinates; warping changes only sample coordinates. The numeric preflight also bounds octave-scaled lattice coordinates, rejecting unsafe combinations rather than clamping. Coordinates passed to the compiled field must be finite and within ±4096 metres.

FastNoiseLite v1.1.1 is pinned by immutable revision and stays private to the engine adapter; see `third_party/FastNoiseLite/README.md` for provenance and the checked unsigned-arithmetic patch. The compiled field is immutable and allocation-free during sampling. Allocation failures propagate to the fatal application boundary. The complete pack compiler and production terrain generator consume these field records.

## Feature templates

`decodeFeatureDefinitions` returns owned templates in canonical key order. A
template contains geometry and attachment markers; eligibility, spacing,
priorities and candidate retries belong to world placement rules. The decoder
does not spawn entities. The document cap is 1 MiB, with at most 128 templates,
128 operations and 64 markers per template. These are implementation safety
caps, not recommended content densities.

All top-level fields below are required. Reservations use half-open local
fine-cell bounds, each coordinate within `[-4096,4096]`, and positive extents
no larger than 4096 fine cells. Negative local offsets are supported. Rectangle
and ellipse operations use the authored bounding box; capsule endpoints are
local fine-cell boundary coordinates and their positive finite radius is in
metres. The complete geometric bounds must fit the reservation. Marker integers
identify cells inside that reservation; their attachment position is the cell
centre. Horizontal reflection capability is explicit on the template.

```toml
schema_version = 1
[features.landing]
display_name = "Landing chamber"
allow_horizontal_reflection = true
reservation_min_x_fine = 0
reservation_min_y_fine = 0
reservation_max_x_fine = 36
reservation_max_y_fine = 18

[features.landing.operations.shell]
order = 10
action = "paint"
shape = "rectangle"
x_fine = 0
y_fine = 0
width_fine = 36
height_fine = 18
material = "sandstone"

[features.landing.operations.room]
order = 20
action = "carve"
shape = "ellipse"
x_fine = 3
y_fine = 3
width_fine = 30
height_fine = 12

[features.landing.markers.spawn]
role = "start"
x_fine = 18
y_fine = 6
```

Operations require unique explicit `order` values in `[0,65535]`, an `action`
(`paint` or `carve`), a `shape` (`rectangle`, `ellipse`, `capsule`), and integer
`x_fine`/`y_fine`. Rectangle/ellipse require positive integer `width_fine` and
`height_fine` and forbid capsule fields. Capsule requires integer `end_x_fine`,
`end_y_fine` and `radius_m`, and forbids width/height. Paint requires a terrain
material reference; carve forbids `material`. Zero-length capsules describe
discs. At least one operation is required; markers are optional and sorted by
key. A marker requires `role`, `x_fine`, and `y_fine`. Roles are `start`,
`traversal_room`, `sealed_pocket`, `resource_anchor`, and `inert`; they are
metadata, not live gameplay objects.

This decoder validates authored geometry and references. It does not yet stamp
terrain or prove spawn support, actor clearance, resource reachability or fluid
containment. Those checks belong to numeric generation and integration.

## World profiles

`decodeWorldProfiles` consumes previously validated material, field, biome and
feature definitions. It returns owned profiles sorted by key. Each document
requires `schema_version = 1`. All profile and rule controls described here are
required unless explicitly identified as conditional; they do not inherit
algorithm tuning from C++ defaults.

Each `[worlds.<profile>]` has these fields:

| Fields | Meaning and domain |
|---|---|
| `display_name` | Nonempty display name, at most 256 bytes |
| `default_wall_material` | Terrain material used by construction |
| `boundary_fine` | Permanent boundary thickness, 1..128 fine cells |
| `min_sites`, `max_sites` | Ordered inclusive site-count bounds, 1..4096 |
| `site_attempts` | Poisson candidate attempts per active site, 1..64 |
| `site_spacing_m` | Positive finite minimum physical separation, at most 4096/3 m |
| `boundary_warp_amplitude_m` | Finite displacement control, 0..4096/3 m |
| `boundary_warp_x_field`, `boundary_warp_y_field` | Named fields, also required when amplitude is zero |
| `start_biome` | Biome enabled by this profile |
| `start_feature`, `start_marker` | Template and its `start` attachment marker |
| `start_x_fine`, `start_y_fine` | Fixed template origin, integer world fine-cell boundary coordinates 0..4095 |
| `start_clearance_m` | Minimum vertical clearance, at least 3 m and no greater than reservation height |
| `start_route_width_m` | Positive minimum route width, no greater than reservation width |

The translated start reservation must fit strictly inside the permanent world
boundary. Start site/spawn coordinates derive from the translated marker centre,
not the template origin. The template geometry authors room dimensions, shell
materials and alcoves; the profile authors their checked minimum guarantees.

At least one `[worlds.<profile>.biomes.<choice>]` is required. Each choice has
`biome`, positive finite `weight`, `required_count` (0..4096),
`depth_min_normalized`, `depth_max_normalized`, `distance_min_m`, and
`distance_max_m`. Normalized depth must have a nonempty increasing interval in
`[0,1]`; distance bounds are increasing or equal within `[0,2048]` metres.
Required counts cannot sum above `max_sites`. Weights are accumulated in choice
key order; overflow and lost normalized intervals fail. Site eligibility and
required-biome placement feasibility are checked by generation.

Two to eight `[worlds.<profile>.start_resources.<key>]` records are required.
Each has `marker`, `material`, `minimum_fine_cells` (1..65536), and positive
`max_distance_m` (at most 2048). Markers must be distinct `resource_anchor`
attachments in the start template; materials must be diggable terrain. The
straight-line marker distance must fit the maximum. This authoring preflight
does not establish the supported actor route or actual adjacent resource count.

Each `[worlds.<profile>.rules.<key>]` requires `kind`, `phase`, and `order`
(0..65535), plus the kind-specific fields below. Rules sort by phase then order;
orders must be unique within each phase. A rule scheduled in another phase fails
validation. Keys are retained as stable random-stream identities.

| Kind | Required phase | Additional fields |
|---|---|---|
| `threshold_cave` | `carve` | `biome`, `field`, `depth_min_normalized`, `depth_max_normalized`, `threshold_min`, `threshold_max` |
| `carve_worm` | `carve` | `biome`, `count`, depth bounds, `length_min_m`, `length_max_m`, `radius_min_m`, `radius_max_m`, `turn_probability` |
| `vein_worm` | `deposit` | Worm fields plus terrain `material` and `host_material` |
| `connect_rooms` | `carve` | `radius_m`, `extra_links` |
| `feature` | `stamp` | `feature`, `biome`, depth bounds, `spacing_m`, `min_count`, `max_count`, `attempts`, `priority`, `reflect` |
| `matter_fill` | `fill` | `feature_rule`, `marker`, particle `material`, `temperature_k`, `bulk_density_kg_m3`; nongas fills also require `fill_height_m` |

All biome references must be enabled by the profile. All depth intervals are
nonempty and within `[0,1]`; cave thresholds form a nonempty interval within
`[-1,1]`. Worm count is 1..1024 per rule, length is positive and at most 4096/3 m,
radius is positive and at most 64 m, and turn probability is in `[0,1]`.
Length/radius minima may equal maxima. Carve worms forbid material fields.
Room connections use positive radius at most 64 m and 0..64 extra links; sealed
pockets are excluded when generating the anchor graph.

Feature spacing is positive and at most 4096/3 m; counts satisfy
`0 <= min_count <= max_count <= 1024`, with positive maximum, 1..64 attempts and
priority 0..65535. A positive minimum is required placement; zero allows a
reported omission. Reflection requires template support. Reservations and
priority conflict handling are generation responsibilities; templates are
resolved here without stamping them.

Initial matter references a feature-placement rule and a `sealed_pocket` marker
in that rule's template. It applies to each successfully placed instance. The
material must use particle representation. Temperature is positive finite K
within float range; bulk density is positive finite kg/m3 at most 1e9. Gas
forbids fill height. Other phases require an exact integer fine-cell boundary
expressed in local metres (`i / 3`, using the nearest representable double), above
the reservation bottom and at or below its top. Signed local heights are
supported. Nearby off-lattice values are rejected rather than rounded. These
records describe unsimulated initial matter; final geometry, containment,
overlap, spans and volume/mass are established by generation.

Safety caps are 16 profiles per 1 MiB document, 64 biome choices, eight start
resources and 128 rules per profile. Aggregate output controls allow at most
4096 worms, 1024 feature instances and one room-connection rule per profile.
Further numeric scratch/output bounds are enforced by the generation compiler.
World-profile decoding feeds the production numeric compiler; the complete-pack loader below validates identity and textures.

## Complete pack loading and CPU validation

`loadContentCatalog(root, diagnostics, context)` runs on the coordinator or a
headless caller. It owns materials, fields, biomes, features, profiles and frozen
CPU images. It validates all five manifest documents before decoding every
required terrain texture. Unique authored texture paths share one decoded image;
there is no missing-image fallback. Paths must resolve inside the canonical pack
root and name regular files. Errors retain the original material assignment
location, qualified property and offending path. Cancellation is checked between
bounded documents and images; allocation failures remain fatal.

The implementation caps each texture at 2048 by 2048 and 16 MiB input, with
64 MiB aggregate input and 128 MiB decoded pack storage. The material cap also
bounds distinct textures to 256. These are safety limits, not content targets.

The complete fingerprint uses FNV-1a and the domain `ppr.content.v1`, schema
version, pack ID, all canonical resolved definitions and referenced texture
source bytes (a length and domain-separated FNV-1a source checksum). Integers
are little-endian, strings/counts have u64 lengths, booleans are one byte,
enums are u32 and doubles use their explicit IEEE-754 bits. Optional fields
include a presence byte. Keys are sorted; ordered records use their validated
phase/order/key sequence. Comments, source locations, document filenames and
texture filenames are excluded. Renaming an asset without changing its bytes
therefore preserves identity. The generator version and metric convention belong
separately in the world descriptor. The material-only decoder returns no complete
fingerprint, so it cannot be mistaken for a validated pack.

`assets/worldgen/default` ships twelve materials, four PPR biomes, five templates,
and two profiles (`frontier` and `hollows`). The second profile changes region
weights, cave density and vein/tunnel geometry. Seven original sRGB textures are
included with provenance. The generator validates geometry and supported starting routes before candidate publication.

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
bounded worm/room/feature geometry, chunk-local terrain rasterization and an
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
version-1 game envelope (`PPRWGEN1`). It stores the exact pack schema/generator
version, profile, unsigned seed, content fingerprint and 3:1 metric, plus the
sorted numeric-ID/material-key manifest (including reserved vacuum ID 0), biome
sites, feature instances/segments/omissions, start/resource results, initial
matter spans/fill heights and baseline validity, generation report, fixed tick
and frequency, and controlled entity slot/generation. Fields are explicit
little-endian integers and IEEE-754 doubles; strings/counts are length-prefixed.
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
continuation. An edited world with invalidated initial matter remains loadable
even when it no longer has its original reservoir containment or starting route;
its preserved matter metadata is labelled an invalidated baseline, not live fluid.


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
persistent checksums and are deliberately absent from checkpoint v1; restored
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

The progress modal shows one bar for the current stage. Terrain composition uses completed generation chunks; candidate graphics staging uses acknowledged staged chunks. Physics preparation, diagnostic capture, GPU readiness, publication, cancellation and cleanup have no measured completion ratio and display an animated indeterminate bar. A full staging count is not a promise that the new world has presented.
