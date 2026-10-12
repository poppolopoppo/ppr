# World-generation overhaul — implementation plan

Status: implemented; executable validation passed with the scopes recorded below.
CLion inspection was unavailable. GPT-6 Luna surveyed the
repository and external references. GPT-6 Astra prepared the plan and provides
independent read-only review. GPT-6.1-sol owns implementation and integration.
Builds and tests stay serialized under the parent coordinator.

Independent GPT-6 Astra review: sound proposal after correcting opacity/coverage
compositing. The review clarified persistent particle ownership, local terrain-edit transactions,
lining precedence, explicit camera transport and checkpoint schema validation.
These clarifications are incorporated below. The coordinator has verified the
unchanged baseline CPU/GPU diagnostics and both production acceptance profiles;
implemented normal/noise and content-schema batches have passed their builds and
focused tests. Remaining implementation gates are tracked below and in
`out/worldgen-overhaul/` evidence logs.

## Intended result

Replace the shipped content with recognizable geological provinces, purposeful
resource distributions, varied caves and contained pockets. Ship 11 biomes and
45 materials through the existing data-driven pipeline. Include normal maps,
metallic/roughness/emissive/opacity/IOR data, four parallax background layers,
and cheap background-only refraction. Lighting, thermal simulation, phase
changes and fluid dynamics are outside this implementation. Persistent gas,
liquid and granular particles are included.

**Accepted requirement:** matter persists through edits without fluid dynamics.
The live world owns bounded chunk-partitioned particle records with stable IDs,
continuous metre positions, material/phase, volume, mass, density and temperature.
Canonical generation spans seed particles; current records, rather than those
spans, are rendered and checkpointed. Terrain edit candidates stage local particle
removal alongside terrain and collider candidates; publication commits both, and
failure retains both previous states. There is no fluid solver or fill-edit UI.
Independent review covers each implemented stage.

## Survey findings

The baseline pack contained 17 materials, seven biomes, seven fields, seven
templates and two profiles (`frontier`, `hollows`). Its pipeline is TOML → owned
content catalog → numeric generation plan → layout/geometry/raster/matter →
owned world → render packet/GridPass.

Existing facilities include Poisson sites, warped Voronoi ownership, depth and
start-distance eligibility, OpenSimplex2/Perlin/cellular noise, fBm/ridged
fractals, progressive domain warp, threshold caves, carve/vein worms, room
connections, templates and contained matter fills. Reuse these facilities.

Boundary warp already works: `WorldGen.Plan.cpp` forwards the fields/amplitude;
`Sim.WorldGen.Layout.cpp::generationBiomeAt` displaces the query before nearest
site lookup. The current profiles use 25 m warp. Boundaries remain hard between
assigned biomes. The landing reservation overrides normal ownership.
Composition bands and deposits use global world depth, not region-relative
depth. Several cave/deposit thresholds repeat, and Lode gives two weighted
choices to basalt, collapsing their intended variation.

Baseline terrain alpha preserved tint × albedo alpha × contour coverage.
Baseline initial matter appeared only in a diagnostic overlay.
The baseline catalog had no normal/PBR/IOR contract. The sim numeric material type carries
physical/generation properties, not appearance; preserve that separation.

## References and adaptation

The user-provided [oni_v22.slang](oni_v22.slang) supplies inspiration for broad
material families, biome-specific distributions, displaced cells, fractal
variants, shells, tunnels and shading properties. Preserve it and all other
user reference files. Its experiment is not evidence of ONI internals.

[Klei's Frosty Planet notes](https://kleiforums.com/game-updates/oni-alpha/622222-r2241/)
describe distinctive Ice Cave, Cool Pool and Nectar regions and configurable
subworld mixing. These are DLC examples. A
[community mirror of subworld data](https://oni.huijiwiki.com/wiki/模块:Data/Worldgen/Subworlds)
exposes composition, temperature, Poisson placement, borders, features and tags;
it is configuration evidence, not proof of noise implementation.

The [Frozen Biome reference](https://oxygennotincluded.wiki.gg/wiki/Frozen_Biome)
describes base-game cold terrain, inset gas rooms and CO₂-lake/solid variants.
Other useful secondary references are
[Sandstone](https://oxygennotincluded.wiki.gg/wiki/Sandstone_Biome),
[Marsh](https://oxygennotincluded.wiki.gg/wiki/Marsh_Biome),
[Jungle](https://oxygennotincluded.wiki.gg/wiki/Jungle_Biome),
[Oily](https://oxygennotincluded.wiki.gg/wiki/Oily_Biome) and
[Magma](https://oxygennotincluded.wiki.gg/wiki/Magma_Biome).
Some wiki pages were available only through search excerpts; exact topology is
not independently established. Adapt their coherent terrain/resource/pocket
themes rather than claiming faithful ONI reproduction or copying artwork.

Use [Khronos material semantics](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html)
and [emissive strength semantics](https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_materials_emissive_strength)
for color-space and metallic/roughness conventions. PPR need not implement the
entire glTF material model.

## Biome recipes

Keep the seven current biome keys and add `verdant`, `mire`, `dunes`,
`petroleum`. Temperatures below are initial game tuning, not equilibrium claims.

| Biome | Host and resources | Characteristic structure and pockets | Temperature |
|---|---|---|---|
| Hearth | Sandstone, dirt, clay; copper ore, coal, algae | Broad rounded chambers, traversable shelves, small water/oxygen pockets, protected landing | 293 ± 8 K |
| Verdant | Dirt, algae, limestone, phosphorite | Branching galleries and bulbous chambers; water/oxygen and small polluted-air pockets | 300 ± 8 K |
| Mire | Slime, clay, shale, coal | Elongated low pools, billowed walls, occasional chimneys; polluted water/oxygen | 305 ± 10 K |
| Dunes | Sandstone, shale, rust, iron ore | Sedimentary bands, tall dry chambers, sand/regolith deposits, CO₂ pockets | 315 ± 15 K |
| Rime | Ice, polluted ice, granite, wolframite | Long shelves, narrow fractures, snow pockets, cold CO₂, water/brine inclusions | 248 ± 15 K |
| Lode | Granite, basalt, ores; native copper/iron/aluminum | Dense angular host, long sparse veins, restricted tunnels, rubble | 318 ± 18 K |
| Karst | Limestone, quartz, amethyst, diamond | Large cellular chambers, narrow necks, nested geodes, clear water | 286 ± 12 K |
| Saltworks | Rock salt, limestone, clay | Evaporite shelves, flattened brine basins, sand beds and CO₂ | 305 ± 12 K |
| Fumaroles | Basalt, sulfur, rust, obsidian | Vertical fissures, chimney clusters, compact side chambers; steam/chlorine/hydrogen | 365 ± 25 K |
| Petroleum | Shale, basalt, coal, diamond | Low basins under dense caps, branching channels; oil/natural gas/CO₂ | 370 ± 20 K |
| Ember | Basalt, obsidian, glass, iron | Dense deep rock, broad hot chambers, sharp fractures; magma and ash | 1100 ± 100 K |

`frontier` emphasizes an upper habitable belt, cold/dry lateral provinces,
mineral middle belt and petroleum/volcanic depths. `hollows` emphasizes connected
karst/mire/salt chambers, thinner bridges and rarer dense mineral provinces.
Their differences must change geometry and placement, not just colors/weights.

Begin tuning around 24–48 sites, 160 m spacing and 15–30 m boundary warp, then
check feasibility and actual coverage in both profiles. Depth/distance
eligibility limits site placement; it does not establish exact cell-level depth
boundaries after Voronoi ownership/warp. Keep important biome counts explicit
and late-game provinces away from the starting neighborhood.

Give each region a dominant host, secondary structural material and localized
resources. Use distinct cave/deposit fields and thresholds. Author roughly two
feature variants per biome plus the landing: nested geodes, pools, chimneys,
ore lenses, rubble beds and capped reservoirs. Sealed pockets stay protected
from traversal connections.

## Focused generator additions

Extend the existing field descriptor with identity-default coordinate scales
`axis_scale_x/y` (proposed 0.125–8) and rotation (−180–180 degrees). Add billow
fractal accumulation using `2 * abs(noise) - 1`, normalized across octaves.
Define octave seeds and coordinate-transform/warp order explicitly. Preserve
existing fBm/ridged behavior and extend lattice/coordinate preflight for the
transforms. Sampling remains immutable and allocation-free.

Add optional per-biome terrain lining material and discrete thickness, with
shipped widths of 1–3 fine cells. Find edge proximity using a bounded chunk-local
biome-ID halo and integer distance transform; specify metric and tie/order
semantics. Avoid a persistent full-world mask or a second nearest-site search
per cell. Suppress threshold caves in lining; deliberate worms, connections and
stamped features may cross it. Preserve world boundaries and start reservations.
Test halo seams and cancellation, and measure added sampling/scratch cost.
Pin precedence: base composition/deposits → lining outside reserved features →
authored cave/worm/connection rules (threshold caves suppressed in lining) →
reserved feature stamps. The protected world boundary remains absolute. A late
lining pass must never reseal an intentional tunnel.

Keep composition bands explicitly global. Directional fields produce interior
strata without a separate region-relative coordinate framework in this scope.

## Material catalog

Preserve existing keys/IDs and clarify native-metal display names. Proposed
45 records:

- 28 terrain solids: existing `basalt`, `sandstone`, `copper`, `iron`, `ice`,
  `masonry`, `obsidian`, `clay`, `limestone`, `salt`, `sulfur`, `crystal`; add
  `granite`, `shale`, `dirt`, `algae`, `slime`, `coal`, `rust`, `phosphorite`,
  `aluminum`, `wolframite`, `diamond`, `clear_quartz`, `glass`, `polluted_ice`,
  `iron_ore`, `copper_ore`.
- Five solid particle records: `sand`, `snow`, `pebbles`, `regolith`, `ash`.
- Five liquids: `water`, `polluted_water`, `brine`, `crude_oil`, `magma`.
- Seven gases: `oxygen`, `polluted_oxygen`, `carbon_dioxide`, `hydrogen`,
  `chlorine`, `steam`, `natural_gas`.

`crystal` can display as amethyst without changing its key. Native metals and
ore matrices have separate IDs and appearance. New IDs need an audited explicit
ledger; suggested ranges are 600–615 terrain, 1100+ granular, 1200+ liquid and
1300+ gas, subject to collision checks. Every material needs a shipped use.
Particle is currently a content category; no actual particle runtime was found
in the inspected simulation/game/render paths. This overhaul must implement
persistent gas/liquid particle state seeded from generation, without a fluid
solver. Terrain remains solid-only.

## Appearance and assets

Retain sRGB RGB plus linear opacity in `color_rgba`. Add:

| Property | Proposed contract |
|---|---|
| metallic / roughness | Finite [0,1], defaults 0 / 1 |
| emissive RGB | Linear factors [0,1], default black |
| emissive_strength | Finite nonnegative multiplier, default 1; GPU-representability checked |
| alpha_mode | opaque or blend; explicit in shipped content |
| ior | Finite 1–3, default 1.5; gases authored near 1 |
| normal_texture | Optional canonical relative path; absence is exact flat normal |
| normal_scale | Finite 0–4, default 1; scales tangent XY before normalization |
| refraction_depth_m | Finite 0–1 effective thickness; default 0 disables distortion |

Ranges are supported tuning domains. IOR/thickness drive a cheap approximation,
not volumetric ray tracing. Metallic/roughness are validated and transported for
future lighting. Emission can show self-emission now without lighting neighbors
or adding bloom; define exposure/output mapping during implementation.

Terrain continues requiring albedo. Allow optional albedo/normal textures and
their defined tiling controls on initial-matter materials without adding terrain
or diggability semantics. Encode tangent normals as RGB mapped to [−1,1], neutral
(0.5,0.5,1); define +world-X tangent, +world-Y bitangent and camera-facing normal
with explicit handedness. Verify displacement direction by rendered tests.

Albedo RGB is sRGB; normals are linear data. Build normal mips by decoding,
averaging and renormalizing, with defined flat fallback for a zero average.
Define zero-vector and back-facing normal handling at base sampling too, and
bound displacement near grazing normals.
Albedo/normals share stable world tiling and gradients. Deduplication distinguishes
asset usage/color space even for identical paths/bytes.

Create original deterministic periodic height/albedo assets, with normals derived
from the same height structures: sediment bands, rock fractures/grain, ore
nodules, facets, ice veins, organic mottling and liquid ripples. Use approximately
128² material maps and 512² backgrounds. Keep suitable current originals; add
reproducible generation scripts and provenance. Do not copy ONI artwork.
All normal/background inputs and mip chains count toward aggregate pack budgets;
make distinct-texture caps explicit rather than deriving them from material count.

## Parallax and refraction

Add a sixth manifest document, `backgrounds.toml`; each profile selects a stack.
Use a small layer schema: order, texture, tint/alpha, repeat period in metres and
parallax factor. Four layers depict distant strata, broad cave silhouettes,
middle formations and near backwall detail. Suggested factors: 0.08, 0.22, 0.45,
0.70. Start with profile-authored stacks; no simulated background world.

Texture coordinates use `uv = (world_xy - camera_origin_xy * (1 - factor)) / period_m`.
The inverse view-projection intersects each pixel ray with world Z=0. Layer factor
scales camera translation; zoom magnifies stable artwork. Derivatives of world
coordinates select texture mips. No random changes,
time animation or per-frame asset regeneration. Camera movement changes constants
and redraws background, not terrain uploads/catalogs.

`PreRenderContext` already exposes device/command encoder before surface rendering.
Use this hook for a GridPass-owned offscreen RenderTarget|ShaderResource backdrop:
compose layers far-to-near, transition to sampled, draw the backdrop on the main
surface, then terrain/current particles, front actors/tools and UI.
The hook does not carry a camera or viewport: pass the matching owned frame's
camera/viewport explicitly to backdrop preparation, preserving existing
visibility preparation and frame ownership.

Refraction samples the immutable composed backdrop, never the active attachment
or foreground objects. Use bounded normal/IOR displacement, conceptually
`normal.xy / max(normal.z, epsilon) * (1 - 1/ior) * refraction_depth_m`, converted
through camera/viewport. Define a screen-space cap and edge sampling. Composite
linear material color/opacity and displaced background with contour/fill coverage
applied exactly once. Separate optical opacity `o` from geometric coverage `c`:
within a covered surface, form `S = o * materialColor + (1 - o) * B_refracted`,
then composite `c * S + (1 - c) * destination`. Thus zero-opacity covered glass
can still distort the backdrop. Do not send precomposed `S` through the current
SrcAlpha blend using `o * c`, which would weight the background twice. Resolve
complementary terrain/matter coverage in one fragment where possible; define
nonoverlapping coverage and draw order rather than summing unbounded alpha.
IOR1/flat/zero-thickness controls must reduce exactly to ordinary compositing.
IOR1, flat normals, zero thickness and opaque materials
produce no distortion. Transparent terrain retains physical collision.

Own target/views/pipelines in the content pass; use completion-safe slots and
retire old size/format resources after their last use. No global GPU idle workaround
or Renderer content ownership. Cover resize, reload, cancellation and partial
initialization. The headless GPU capture host must use the same background and
foreground composition. Resource dimensions follow the draw viewport.

Persistent particle matter remains visible through terrain edits. Remove the
current global baseline-invalidation rendering behavior. Generation regions/spans
remain reproduction metadata; particles own current matter state. Seed one
particle at each canonical eligible fine-cell vacuum sample and preserve IDs after
removals. Correct the final volume only within bounded accumulation roundoff, then
derive every particle mass coherently as volume × density. Region volume/mass totals
agree within a relative `1e-8` accumulation tolerance. Terrain edits preserve every particle during pure digging. Newly added solid
terrain removes a point particle only when the candidate contour covers its
continuous center. Changed cells and their contour halo bound reevaluation; they
are not a destruction mask. Visual support is clipped independently of physical
volume. Preflight particle counts against coherent CPU/GPU/checkpoint byte caps
before allocation; allocate only occupied chunks and capture changed/visible
batches instead of copying all particles each frame.
Stage those removals in the same candidate as terrain/colliders; matching GPU
publication commits the candidate, while failure or cancellation retains prior
terrain and particles. Checkpoint v2 serializes current records and the next ID,
validates finite physical values/materials/bounds/unique IDs and bounded counts,
and restores them without re-seeding. No fill-paint/erase UI is assumed.

## Identity and compatibility

Recommend pack schema v2, new canonical material/content fingerprint domains and
a generator-version increment for changed semantics. Hash resolved new fields,
ordered background layers and usage-aware asset bytes canonically. Preserve
comment/record-order independence and filename independence when semantics/bytes
are unchanged. Appearance stays game-layer; sim receives only necessary numeric
generation controls.

Existing seeds intentionally produce new worlds. Preserve old numeric IDs; reject
old checkpoints with explicit schema/generator/content mismatch. No migration is
proposed. Checkpoint envelope v2 adds an explicit particle record section; the low-level sim
snapshot format remains unchanged. Descriptor checks use the supported schema and
generator constants. Fixture hashes are updated only from measured canonical
identities; separately purposed legacy numeric fixtures retain their meaning.

## Implementation stages and source seams

1. Capture baseline; settle schema/material/background contracts and compatibility.
2. Add field transforms/billow and biome lining with focused numeric tests.
3. Author full catalog, biomes, profiles, templates and original assets.
4. Extend material GPU payload, normal sampling and current particle presentation.
5. Integrate parallax backdrop and background-only refraction.
6. Tune visuals, measure costs, run integration gates and independent Astra review.

Likely touched areas: `assets/worldgen/default`, content declarations/decoders and
`Content.Pack.cpp`, `WorldGen.Plan.cpp`, sim worldgen Noise/Plan/Layout/Raster,
render packet/diagnostic code, GridPass catalog/publication/binding code,
`assets/shaders/grid_tiles.slang` and backdrop shader, GPU capture/CPU exports,
checkpoint validation/tests, worldgen documentation and owning codemaps. Confirm
exact current source registrations before adding module files.


## Implemented particle and resource contract

The world owns at most 1,048,576 logical live particle records: 64 MiB at the
64-byte CPU stride. Checkpoint records use explicit 59-byte encoding, bounded by
the existing 256 MiB whole-checkpoint limit before allocation. GPU instances use
32 bytes each. Occupied chunks allocate their counted seed records, and immutable
shared batches preserve frozen frames/checkpoints through edits. These are logical
payload bounds: container capacity, metadata, translated render records and retained
older snapshots add memory. They are not a claimed process-memory ceiling.

The present static model permits one particle per owning fine cell. Continuous
positions, physical volume, mass, density, material, phase, temperature, stable ID
and the next ID survive checkpoint restore directly. No historical span re-seeding
occurs. Particle presentation owns a half-open fine cell, clips its support to the
shared terrain contour, and normalizes fill by the canonical empty cell area times
reference thickness. Conservative support reaches the entire owning cell even for
an off-center restored position. Position controls material sampling and the
new-solid center-removal test; this model has no overlap or movement solver.

Pure digs retain all IDs, physical values and immutable batch pointers. Added solid
terrain evaluates only nearby candidate contour support and removes particles whose
continuous centers become covered. Candidate batches commit with terrain/colliders
after complete matching publication; empty, partial, stale or failed publication
keeps previous ownership. Postcommit wake failures clear the committed candidate
before propagating the error. Headless authored edits use the same local transaction.

Color and backdrop maps decode as sRGB; normals decode as linear data. Normal mips
renormalize signed vectors, including level zero, with safe +Z fallback. Material
appearance remains outside the simulation module. Frame packets share immutable
particle batches and translate changed chunks, avoiding a full particle copy each
frame. Current particle diagnostics and inspection follow these records.

The original 128² maps exposed a pre-existing buffer allocator routing defect:
page-size multiples incorrectly selected a single fixed page. The buffer selector
now selects SmallPage/HugePage only for exactly one block and uses GPA for larger
multiples. Fixed-page allocator assertions remain intact; focused buffer tests cover
one/two small and huge pages, content, ownership transfer and release.

## Acceptance evidence

Baseline and candidate: both profiles × seeds 1, 42, 1337. Retain identities,
CPU material/biome/temperature/matter maps, fixed cameras, GPU closeups and
interactive acceptance reports. Baseline CPU captures (both profiles × three seeds),
GPU captures, and both production acceptance profiles passed. Implemented normal/noise
and schema batches passed builds and focused tests. The complete simulation suite passed after particle/lining integration, including
the corrected per-worker raster halo scratch bound (52.63 s). Core buffer tests
passed all 39 leaves, including exact and multiple page-size routing (79 ms).
The joint build passed after the bounded renderer flag fix and an MSVC module
signature workaround: the driver exposes primitive profile/thickness presentation
values rather than the packet implementation deserializing nested descriptors.
Grid/packet tests passed all 34 app leaves; focused image mip tests passed.
Pack loading now succeeds after the allocator correction. The pack seed matrix
exposed an uncovered shallow biome eligibility interval; Hearth's minimum depth
was extended to zero without weakening required biome counts. The same fixed
profile/seed matrix then passed (14.82 s). Checkpoint tests exposed a new fixture
phase error: preserved Water ID 1001 was incorrectly labeled gas. The fixture now
uses liquid, and its malformed-phase case uses gas; runtime validation is unchanged.
The repaired checkpoint group passed all four leaves (68.05 s), including local
particle preservation and complete/partial/stale publication behavior. Full pack tests then passed
11 of 12 leaves in 311.60 s; the sole failure was the old shipped identity pin,
now updated to the measured `1acd645a681bf61a`. All identity mutation checks passed.
The 174.57 s mutation leaf now uses two valid opaque 1×1 PNGs only in its temporary
pack, retaining all 19 semantic and three source-byte mutation checks; its mutation timing
recheck passed at 84.06 s (95.33 s with the shipped-pin leaf). The complete-catalog test retains original texture/mip checks.
The prepared-session smoke checks real seeded particles against the already
generated per-material matter totals, selected cell count and phase mix without
additional world generation. Its Dev recheck passed; final pack generation,
optical GPU, lifetime/cost and canonical Dev matrix evidence appears below.
Diagnostic export still averages all 4×4 fine-cell samples per pixel. Debug
capture time is not compared directly with the Release baseline. Current candidate
interactive results and remaining preset/backend rechecks are recorded below;
no unrun gate is considered passed.

- Content: invalid factors/IOR/assets/paths/budgets, color-vs-data decoding,
  normal mips, deterministic identity, unknown fields and all shipped references.
- Generation: serial/parallel equality, stable seed/order semantics, transformed
  coordinate limits, billow determinism, lining seam/order behavior, protected
  start/boundary, cancellation/draining.
- Quality: region coverage, material distributions, cave fractions and omissions;
  verify each promised region character across the seed matrix and inspect
  degenerate/absent regions. Avoid a mere palette recolor.
- Start: actor footprint, route width, exposed resources and pocket containment.
- GPU: CPU layout/stride assertions, actual shader compilation/pipelines/readback,
  flat normal/IOR1/alpha endpoints, color spaces, refraction direction, fill
  clipping, chunk seams and parallax translation/zoom sequences.
- Transactions: New Game cancellation/replacement, edit success/failure/rollback,
  invalidated checkpoint restore, resize and resource retirement.
- Cost: identical workload before/after generation times, known scratch/retained
  payload, process peaks, GPU resource bytes/backdrop cost/frame cadence and
  settled cache counts. Record samples/spread; invent no global performance budget.
- Repository gates: formatter/style, affected suites per stage, full twelve-entry
  matrix for cross-cutting rendering, required three-loop affected lifetime/render
  suites, normal Windows preset matrix and staged IDE inspection. Exercise D3D12
  and Vulkan where available; disclose unrun backends/gates. Use one persistent
  Insiders vcvars build shell and serialized builds/matrix invocations.

## Review decisions

Recommended scope is the 11-biome/45-material roster, intentional new-world
identity incompatibility, original map assets, four-layer parallax and single
background-sample refraction. Persistent gas/liquid particles through terrain
edits are required, without fluid dynamics. The user approved implementation. Work is staged with serialized validation
and independent read-only correctness review; no unrun gate is considered passed.

## Data quality iteration

The first candidate Frontier/42 CPU export passed in Debug (131.52 s), with 40
sites, 73 features, 108 initial-matter regions and 3,057 spans. Visual inspection
found repeated cellular karst disks, dense province-wide slit patterns and shared
cross-shaped landmarks. A retained pre-naturalization pack and capture preserve
that evidence. The next data-only batch replaces cellular karst carving with
warped simplex/fBm, uses coarser composition/deposit and moderate cave anisotropy,
and authors separate asymmetric connected lobe outlines for regional templates.
No texture pixels, material IDs, protected landing, site constraints or placement
counts change. Generated carve bounds retain four-cell reservation margins and
all markers remain empty in a source-data audit. The real content loader accepted
the naturalized pack, with measured fingerprint `83727a0a2b51b4ee`; the shipped pin
asserted that intermediate value. All identity-mutation literals still match the
authored documents. Release Frontier/42 and Hollows/42 overviews passed; broad
cave shapes replaced the repeated disks and zebra fields. Hollows exposed a
conspicuous straight global room-connection graph. The final authored correction
omits that optional rule from both profiles while preserving curved worms,
all local traversal templates, sealed fills and the protected start contract.
Remote galleries consequently do not promise one globally connected network.
The generic connection generator and its dedicated tests remain available.
The Dev application validator accepted the corrected source pack with measured
identity `c70c6df6a40ffaaf`; the shipped pin now asserts that final value. All six
final Release CPU previews passed: Frontier and Hollows, each with seeds 1, 42
and 1337. Every preview contains all 11 biomes and reports zero omitted features.
The final seed42 material overviews were inspected; the straight web is gone.
A temporary
full recipe regeneration reproduced all 105 generated files, including 98 PNGs,
byte for byte; a data-only regeneration left all 106 pack files unchanged.
All nine canonical non-game CTest entries passed serially (1036.24 s). The full
game suite exceeded its previous 300 s CTest timeout with every completed leaf
passing. A direct complete `game.tests --loop1` run then passed all 157 tests in
357.80 s aggregate (358.173 s wall). Only that suite's source-CMake timeout is now
600 s to accommodate its real shipped-content and checkpoint workload; validator
timeouts, assertions and test selection remain unchanged. The canonical game
CTest rerun passed in 360.23 s. Together with the nine non-game entries and both
worldgen validators (7.45 / 7.36 s), all 12 canonical Dev CTest entries passed.
The original 300 s timeout remains recorded as a repaired test-metadata failure.
Required same-process Dev three-loop checks also passed: all 88 affected app
leaves (`app/frame_fence,app/render_lane,app/grid_pass,app/sim_packet`), all 27
affected asset leaves (`asset/render/gates,asset/image/mips`) and all five
checkpoint leaves (`game/checkpoint`) passed in each of three loops. Aggregate
wall times were 574.932 / 685.288 / 171.995 seconds respectively. Filters, loop
counts, exit codes and log paths are recorded in
`out/worldgen-overhaul/final-three-loop-results.json`; the corresponding
`final-three-loop-app.log`, `final-three-loop-asset.log` and
`final-three-loop-checkpoint.log` retain leaf-level evidence.

The Dev GPU-validation prepared-session smoke passed in 305.295 seconds,
including 116,868 persistent particles, 10,359 liquid records, 103,824 gas
records and all 17 generated matter materials. The remaining 2,685 records are
granular. `out/worldgen-overhaul/final-prepared-smoke-dev.log` retains the
prepared-session, edit, agent, build and GPU smoke checks. Deliberate rejection
diagnostics in that log belong to negative checks; the completed smoke reports
PASS.

The original candidate Frontier/42 Release interactive run failed its 300-second
bound in textured-preview capture phase 5, reporting `INCOMPLETE: shutdown
preceded sequence completion` in
`out/worldgen-overhaul/final-acceptance-frontier-release.log`. Investigation found
that two preview resource lifetimes could share a world generation: retirement
then matched and detached roots from the newer preview. The repaired root identity
uses both job and generation. Independent read-only review accepted the repair.

The exact same Release acceptance commands subsequently passed both profiles:
Frontier/42 in 38.8196 seconds and Hollows/42 in 37.5921 seconds. Both completed
explicit phases 1–8 and settled to one retained atlas, 98 texture entries and 98
texture slots after preview switches and reloads. Commands, exit codes and log
paths are in `out/worldgen-overhaul/repaired-acceptance-results.json`; the logs
are `repaired-acceptance-frontier-release.log` and
`repaired-acceptance-hollows-release.log`. Interactive GPU timestamps were
unavailable, and native input latency was not measured. These scripted passes
cover their explicit production-path checks; they do not establish those
unmeasured properties.

The full twelve-entry Dev matrix, required same-process three-loop groups and
prepared-session smoke above predate this final retirement-identity repair.
After the repair, all 88 affected Dev app leaves passed again (188.88 seconds
aggregate, 189.5128 seconds wall; `out/worldgen-overhaul/retirement-app.log`).
Post-repair resource-leaf checks passed all three same-process loops on both
D3D12 (135.5070 seconds wall) and Vulkan (10.6402 seconds wall). The exact filter,
backend selector, loop counts and successful exit codes are in
[retirement-check-results.json](../out/worldgen-overhaul/retirement-check-results.json);
leaf logs are `retirement-resource-d3d12.log` and
`retirement-resource-vulkan.log`.

All six Live targets built successfully in 52.2001 seconds:
[final-live-build.json](../out/worldgen-overhaul/final-live-build.json) records
the command and `final-live-build.log`. Focused Live app, optical/resource/mip
and checkpoint checks passed in 0.4893 / 4.4235 / 5.1434 seconds wall respectively.
[final-live-focused-results.json](../out/worldgen-overhaul/final-live-focused-results.json)
retains their exact filters and successful exit codes; logs are
`final-live-app.log`, `final-live-asset.log` and `final-live-checkpoint.log`.
These are focused Live rechecks, not a second full canonical matrix.

The final source-format check passed across 50 changed C++ files with no rewrites.
Focused semantic-style checks reported zero blocking findings, with 31 advisories
for the production batch and four for the final test refinement. CLion tools were
unavailable, so staged IDE inspection is unrun and is not reported as passed.
Earlier GPU closeup measurements below retain their narrower
completed-submission scope. Interactive GPU timestamps and native input latency
remain unavailable/unmeasured as stated above.

GPU closeup manifests now record retained texture, particle and backdrop payload
counters and completed submission-matched GPU timestamps when supported. They
report unavailable rather than attribute an older submission's timing to a view.
CPU submission, host completion wait and GPU command time remain separate;
timestamps cover the complete submitted backdrop preparation/terrain/particle work,
not an isolated terrain pass. Focused D3D12 and Vulkan optical/resource readbacks
passed without subresource warnings. Both final seed42 real-world GPU captures
passed, including the telemetry collection. Across the eight 512×512 views,
completed submission-matched GPU frame samples ranged from 0.159744 to 0.297696
ms. Retained decoded textures used 19,049,000 bytes and the three backdrop targets
used 6,291,456 bytes. Frontier retained 116,868 particle instances in 3,739,776
buffer bytes; Hollows retained 155,824 instances in 4,986,368 bytes.

The matched Release raster samples are recorded in
`out/worldgen-overhaul/candidate-final/quality-and-cost.json`:

| Profile | Baseline raster seconds, seeds 1 / 42 / 1337 | Final raster seconds, seeds 1 / 42 / 1337 |
| --- | --- | --- |
| Frontier | 2.336 / 2.426 / 2.490 | 2.089 / 2.033 / 2.521 |
| Hollows | 2.740 / 2.548 / 2.612 | 3.078 / 2.643 / 2.756 |

Known retained generation payload was 134,738,344 bytes for Frontier and
135,083,216 bytes for Hollows; the largest reported individual stage scratch
payload was 3,162,064 bytes. Sampled process peaks in `invocations.json` were
approximately 180–182 MB for CPU previews and 747–748 MB for GPU capture runs.
These are representative workload samples, not an optimization claim or a total
VRAM measurement. Raster timings exclude image export and GPU capture; process
sampling and pass payload counters measure different scopes.
