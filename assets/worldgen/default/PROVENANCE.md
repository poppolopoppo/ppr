# PPR Frontier descriptor pack — original content and asset provenance

All 134 referenced PNGs are original procedural artwork made for PPR. No ONI artwork,
textures, code, extracted game data, or other third-party assets are included.
The user-provided `docs/oni_v22.slang` and references listed in
`docs/worldgen-overhaul-plan.md` informed broad geological themes only. They
remain preserved as references and are not generator inputs.

The schema-1 `pack.toml` explicitly indexes 513 authored descriptors. Elements
own physical properties; Materials reference Elements and compose appearance;
Texture descriptors name the existing binary sources. The two Worlds own
explicit palette bindings and reference standalone Rules. Migration preserves
the prior scalar tuning, palette IDs/order, semantic aliases and all 134 PNG
bytes at migration time. The art refinement below subsequently changes selected
appearance values and PNGs. Rule phases still derive from payloads. This is a breaking source-format
change with no seven-document loader. Historical validation and measurements
below do not establish acceptance of the descriptor migration.

Run `python scripts/Generate-WorldgenAssets.py` to generate missing PNGs with Python
3.11+ and no external packages. It never rewrites TOML. Use `--material <key>` to
select textures and `--overwrite-textures` for deliberate regeneration.
Use `--background frontier --background hollows` to select the two background
stacks without touching material maps; combine these selectors with `--material`
to regenerate both selections. No selectors retains the missing-assets default
for the complete pack. Invalid selector keys fail before output creation.
SHA-256-derived semantic-key seeds, domain-warped periodic lattice noise and
jittered cellular fracture planes keep every tile periodic in X/Y. Compression is
deterministic for fixed Python/zlib; decoded pixels define artwork.

Each material has a 128 x 128 RGBA albedo and normal map. Albedo pigment
structures multiply the catalog sRGB tint. Albedo/background PNGs carry sRGB
chunks; normals have no sRGB chunk and are linear data. Normals derive from
wrapped central differences of the same height field used by albedo. They
encode normalized `(-dH/dX, -dH/dY, 1)` mapped to [0, 1], with row zero at world
Y zero, +X tangent, +Y bitangent and camera-facing +Z. Normal scale is a catalog
parameter. Runtime mips must decode, average and renormalize normal vectors;
the script authors base levels only.

Sandstone uses rounded sediment masses with subtle bedding and fine grain;
sand has smaller rounded grains, and regolith uses cohesive orange clay clods
with a fine pitted skin. Shale retains thin fractured laminae, and clay rounded
clumps. Organic materials use mottling; other granular material coarse/fine
grain; rock cellular fractures; iron/copper ores coal-dark rounded hosts with
sparse cool grey-blue mineral nodules; crystals irregular facets;
ice veins; metals hammered/brushed grain; liquids crossing ripples. Gases have
low-frequency mottling and zero normal scale. No animation or live fluid solver
is implied by these assets or initial-fill recipes.

Each background stack contains four original 512 x 512 RGBA layers: distant
opaque strata, cave silhouettes, middle formations and backwall details. The
latter three have transparent gaps. Parallax factors are 0.08/0.22/0.45/0.70.
The desaturated charcoal/slate Frontier and muted violet-grey Hollows palettes
use broad stepped strata behind foreground sediment and mineral pockets. Layer
coverage, parallax descriptors, fixed world anchor, and above-surface sky behavior
are unchanged; the stacks still support background-only refraction.

## Environment art refinement

The seven refined semantic keys are `sandstone`, `sand`, `regolith`, `iron_ore`,
`copper_ore`, `crystal` and `water`. Both shipped worlds reference these same
Material descriptors. Their catalog tints are respectively `D9B574FF`,
`E4C486FF`, `D97843FF`, `727F8CFF`, `8B8981FF`, `9251E58A` and `36D4E570`.
Original deterministic relief and pigment variation provide granular sediment,
sparse ore nodules, violet crystal facets and gentle cyan water ripples.
Sandstone, sand, regolith and both ores use overlapping jittered elliptical
pebbles at three grain scales, with varied radius, aspect, orientation and relief.
Compact rounded height profiles leave real contact cavities in a continuous
matrix; pigment blends over the same footprints rather than partitioning the
surface into flat nearest-cell polygons. Sand uses smaller loose grains, clay
partly fuses coarse clods into its matrix, and ore restricts smaller irregular
mineral inclusions to sparse periodic growth pockets. Normal maps derive from
this same mixed-grain height field. These are source-art refinements; swatches
and diagnostic normal lighting do not establish final in-scene aesthetic acceptance.
No external illumination, cast shadows or amber lamp lighting is baked into
albedo. Simulation Elements, geometry recipes, palette bindings, texture sizes,
opacity, roughness and normal scales are preserved.

Amethyst has emissive RGB `(0.30, 0.07, 0.65)` at strength `0.6`; water has
`(0.02, 0.38, 0.45)` at strength `0.35`. These are restrained material-wide authored
factors for the lighting integration, not measured light output or a performance
claim. The existing catalog has no per-texel emissive or metallic mask: crystal
facets do not selectively emit, and cool ore nodules remain diffuse pigment with
the existing material-wide metallic value of zero. Local lamp transport, bloom,
and final exposure are separate renderer work. These source assets alone do not
establish an in-engine reference match or the RTX 2080 frame budget.

There are 134 texture paths/usages. Base decoded RGBA bytes total 16,646,144
(126 x 128² x 4 plus 8 x 512² x 4). Complete ordinary mip chains total 22,194,680
bytes. These source-art sizes exclude GPU overhead and offscreen backdrop
memory and make no frame-performance claim.

## Semantic material identity

Qualified pack/key/kind names own stable asset identity. Worlds explicitly bind
semantic Material aliases to numeric IDs, with vacuum fixed at zero; these
bindings participate in numeric World identity. Materials remain distinct even
when they share one Element. Display labels remain unchanged (copper/iron are native
metals; crystal is amethyst). Existing explicit numeric test fixtures are separate
from this pack. The texture pattern families are keyed by these semantic names.

## Authored structure

Eleven biomes combine dominant hosts, secondary structure, localized deposits,
anisotropic field families, 1–3 fine-cell linings and initial matter.
Temperatures, densities and optical constants are game tuning rather than
measured equilibrium or optical data. Composition uses global depth bands.

Cave fields use broader warped chambers and fractures, with moderate anisotropy
in ice, evaporite and petroleum provinces. Karst cavities use warped simplex/fBm
rather than equal-radius cellular disks. Composition and deposit scales preserve
large host masses and localized resource pockets.

Twenty-two regional templates cover shelves, branches, low basins, chimneys,
nested geodes, ore lenses and capped reservoirs. Each has independently authored
overlapping asymmetric chamber lobes; carved outlines retain at least four fine
samples to the reservation boundary. Sealed anchors exclude room connections. Three unfilled traversal galleries provide local chambers. The shipped profiles omit the global straight room-connection graph, so remote galleries are not guaranteed to form a single network; curved worms and the protected local start route remain. The
original landing reservation, spawn/stone/ore markers, 54-cell stone/copper
volumes, route width and coordinates remain unchanged.

Frontier uses 32–40 sites at 160 m spacing, an upper habitable belt, compact
tunnels and dense mineral provinces. Hollows uses 40–48 sites at 140 m spacing,
wider karst/mire/salt depth eligibility, larger/more frequent tunnels, broader
cave thresholds and more local traversal galleries. Important biomes and both
regional feature variants require placement rather than silently disappearing.
The historical v2 pack identity was `c70c6df6a40ffaaf`. Its Release CPU previews passed for
both profiles with seeds 1, 42 and 1337: all 11 biomes appeared and no feature was
omitted in each preview. Both seed42 GPU closeup captures passed. Evidence and
representative resource/cost measurements are retained under
`out/worldgen-overhaul/candidate-final`; the implementation plan records the
measurement scopes and remaining validation gates. Before the final renderer
retirement-identity repair, all 12 canonical Dev CTest
entries passed after the game-suite timeout was corrected from 300 to 600 seconds.
Required same-process Dev three-loop app, asset and checkpoint checks passed;
`out/worldgen-overhaul/final-three-loop-results.json` records their filters and
logs. Dev GPU-validation prepared-session smoke also passed with 116,868
persistent particles and all 17 generated matter materials.

The original Frontier/42 Release interactive run failed its 300-second bound in
textured-preview capture phase 5. The independently reviewed renderer repair
distinguishes resource lifetimes by job and generation. The same Release commands
then passed Frontier/42 in 38.8196 seconds and Hollows/42 in 37.5921 seconds, with
phases 1–8 complete and one retained atlas plus 98 texture entries/slots after
settlement. All 88 affected Dev app leaves passed again after the repair.
Interactive GPU timestamps were unavailable and native input latency was not
measured; the recorded GPU closeup measurements retain their narrower scope.
Post-repair resource checks passed three same-process loops on D3D12 and Vulkan.
All six Live targets built; focused Live app, optical/resource/mip and checkpoint
checks passed. Final format and blocking semantic-style checks passed. CLion
inspection was unavailable and remains unrun. The
[implementation plan](../../../docs/worldgen-overhaul-plan.md#data-quality-iteration)
preserves the original failure, repaired command/log identities and final
`retirement-check-results.json`, `final-live-build.json` and
`final-live-focused-results.json` evidence indexes.


### Authored optical metadata, 1 November 2026

All 63 shipped material descriptors now explicitly author version1 optical
transport in `appearance.transport`. Existing linear emissive RGB and strength
are unchanged. Transport uses source function S=RGB*strength; opaque emits S once,
absorbing uses authored sigma per world metre and j=sigma*S, and vacuum has zero
sigma/source. These are artistic starting coefficients, not measured optical
properties or final brightness. Alpha, IOR, texture alpha and physical phase do
not choose transport at runtime.

Named clear/quartz/glass/diamond/ice/crystal exceptions and water/brine/polluted
water/crude oil are absorbing. Steam, polluted oxygen and chlorine are absorbing;
oxygen, hydrogen, natural gas and carbon dioxide explicitly use vacuum. Remaining
rock/granular/metal/colored mineral/slime materials and magma explicitly use
opaque transport. Magma keeps its emissive source despite visual blend; crystal
and water retain their existing violet/cyan sources. The selected lighting plan
records the exact initial coefficients. No texture generation or simulation
properties changed in this metadata batch.

## Authored playable lighting showcase

The three `showcase_*` material rows are explicit opaque solid constructions for
the bounded playable cavern. They reuse the shipped magma/crystal/water texture
assets, but their selected material IDs, solid element, opacity and transport
are authored independently; they are not liquid water or measured optical media.
Amber uses linearized sRGB FFCA75 at source strength2.5, violet(.30,.07,.65) at.6,
and cyan(.02,.38,.45) at.35. These are artistic starting values, not a claim of
physical calibration or final scene brightness. Existing material rows are unchanged.

Only the `lighting_showcase` World palette selects these rows. The existing
frontier/hollows palette IDs and capture proxy bindings remain unchanged.


## Showcase stone backwall and cut-face presentation experiment

Only `lighting_showcase` selects `background_lighting_showcase`; frontier/hollows
background descriptors/textures and the old headless scene recipe are unchanged.
The dedicated four-layer stack retains the established ordering, alpha masks,
repeat distances and decorative parallax. Its generated RGB is stone albedo,
centred near0.18 linear with restrained strata/cool variation; the generator
encodes linear values to sRGB explicitly. No darkness or source glow is baked.
Actual background output remains albedo multiplied by the world incident field.

Regenerate these four PNGs with
`python3 scripts/Generate-WorldgenAssets.py --background lighting_showcase --overwrite-textures`.
The source-authoring freeze intentionally leaves generator execution and bitmap
hashes to the validation owner before staging/build/capture.

The native-reference foreground shader separately declares a neutral0.04
normal-modulated render-only cut-face incident term. It is multiplied by terrain
reflectance; own S is still added once. It never enters the world field, tracing,
feedback, gameplay, backwall or legacy rendering. Matched actual-playable A/B
captures use isolated staged copies with exactly `kNativeCutFaceIncident=0` versus
`0.04`; camera, EV3, source strengths and all albedo assets remain identical.
This is a bounded presentation experiment, not GI/physical/performance admission.


## Playable industrial service bay

Only `lighting_showcase` adds material IDs67..70: housing, raised tread,
horizontal pipe and vertical pipe. They reference the existing iron element,
use opaque transport with S=0 and explicit repeat sampling, and have separate
128x128 albedo/normal pairs produced by `Generate-WorldgenAssets.py`.
Albedo is neutral pigment authored near0.32..0.43 linear and encoded to sRGB;
rivets, seams, pipe unions and tread bars share analytic height with their normal
maps. No lamp glow, directional highlight or baked illumination is painted.
Pipes swap world-XY height coordinates for orientation. No other profile's
material, sampler, source bitmap or background is changed.

The native small-scene Grid path currently uses mapped diffuse plus the accepted
0.04 cut-face key; its metallic/roughness metadata does not establish specular.
These new rows remain within unit reflectance and add no source energy. The
existing65504 response admission is unchanged, not bypassed by this art phase.
The cyan tank insert remains an explicitly opaque emissive proxy, not liquid or
glass. Its top/right apertures are actual vacuum cells.

Generate exactly the eight new PNGs before content preparation/staging:

```sh
python3 scripts/Generate-WorldgenAssets.py --material showcase_housing --material showcase_tread --material showcase_pipe_horizontal --material showcase_pipe_vertical --overwrite-textures
```

Outputs are `textures/showcase_{housing,tread,pipe_horizontal,pipe_vertical}.png`
and each matching `_normal.png` below this pack. Source authoring does not execute
the generator; the validation owner records generated hashes, builds and captures.
Existing ready A/B images remain the pre-bay baseline, with camera/EV/source
strengths preserved. World occupancy changes require fresh field/capture evidence.

## Physical pipe normal correction

On 3 November 2026 the coordinator accepted one incremental correction after
CPU analytic/export QA and inspection of both actual 4K door frames. Pipes show
clear cylindrical shading, visible collar bands and cool/violet reflections;
fixed stone/backwall regions remain unchanged. This accepts the pipe-normal
correction, not complete environment/art/performance qualification. The separate
v4 sandstone remains a comparison asset and is not promoted by this change.

Only these validated offline normal PNG bytes are copied into canonical assets:

| Source PNG | SHA-256 |
|---|---|
| `textures/showcase_pipe_horizontal_normal.png` | `0727b4f09d63e757e942280b51b0cb449a6a5eeb3b22fca104e0173e4bcd648a` |
| `textures/showcase_pipe_vertical_normal.png` | `d93302c7312867da2b8f118b08c79bc10a8dc749ca8595134e27f583684860fb` |

Both pipe albedos, every descriptor, period 1/3 m, normal_scale 1, repeat
sampling, material IDs 69/70, masks, geometry, optics, source S and BRDF remain
unchanged. No stone, pack or other runtime payload is replaced. Files remain
128² RGBA8 data PNGs with alpha 255, no colorimetry tags, OpenGL +X/+Y/+Z and
row 0 at world Y=0. The original x/128,y/128 sampling phase and encoder are retained.

The generator ties `PIPE_TUBE_HEIGHT_AMPLITUDE=.30` to the existing tube term
and `PIPE_RADIUS_TO_PERIOD=.5` to the physical radius contract. Only the two
pipe keys regenerate central differences with
`(size/2)*(PIPE_RADIUS_TO_PERIOD/PIPE_TUBE_HEIGHT_AMPLITUDE)`, or 106.66666666666667
at 128². Every other material retains its previous factor 5. The sampled field
still contains the same tube, `.11` triangular union/collar and `.003` scratch;
no coefficient, albedo formula or phase was retuned.

With radius=P/2 and physical height scale H/P=.5/.30=5/3, P=1/3 m corresponds
to 166.7 mm tube radius, 61.1 mm collar relief and 1.67 mm scratch peak-to-valley.
The scale applies to the complete height field. The 53-cycle scratch is close
to the 128² sampling limit; this correction does not claim to repair that
sampling. The source height repeats across the cylinder silhouette, but its
derivative has a cusp. Periodic central samples cancel the tube slope at the
exact seam, and filtering opposing edge normals can create a front-facing
strip. No UV/sign flip, seam smoothing or false C1 seam guarantee is introduced.
Normal mapping changes neither silhouette nor geometric shadows.

The offline source/export/QA and actual-image comparison are preserved under
`out/diagnostics/environment-lighting-20261102/v22-reassessment/material-lookdev/pipe-normal-physical-authoring/`.
CPU QA established exact original float64 heights, full-field physical-stencil
PNG bytes, tube-only interior controls, transpose plus R/G orientation, data
metadata and positive-Z/quantization-bounded unit normals. The original pinned
generator, SHA-256 `9310f4b196b91af0e1f1dfc913a5c233de548f8e420f8dabaf875c1f56a709ef`,
is retained at `source-promotion/before/scripts/Generate-WorldgenAssets.py` in
that diagnostic directory for historical trial reproduction; old trial pins
remain unchanged and intentionally reject the newly corrected canonical script.

Canonical generator equivalence against frozen heights/PNG bytes and normal
app-target asset synchronization belong to the validation owner after other
tests finish. No regeneration, build/staging or native run is performed by the
source-promotion author. Later deployment/host/zoom/mip behavior and complete
scene aesthetics/performance retain their own validation scope.
