# PPR Frontier v2 — original content and asset provenance

All 98 PNGs are original procedural artwork made for PPR. No ONI artwork,
textures, code, extracted game data, or other third-party assets are included.
The user-provided `docs/oni_v22.slang` and references listed in
`docs/worldgen-overhaul-plan.md` informed broad geological themes only. They
remain preserved as references and are not generator inputs.

Run `python scripts/Generate-WorldgenAssets.py` to reproduce all six TOML
documents, manifest and PNGs with Python 3.11+, without external packages.
`--data-only` regenerates recipes without rewriting images. SHA-256-derived
seeds, domain-warped periodic lattice noise and jittered cellular fracture planes
make every tile periodic in X/Y. PNG compression is deterministic for a fixed
Python/zlib implementation; decoded pixels define artwork.

Each material has a 128 x 128 RGBA albedo and normal map. Neutral albedo
structures multiply the catalog sRGB tint. Albedo/background PNGs carry sRGB
chunks; normals have no sRGB chunk and are linear data. Normals derive from
wrapped central differences of the same height field used by albedo. They
encode normalized `(-dH/dX, -dH/dY, 1)` mapped to [0, 1], with row zero at world
Y zero, +X tangent, +Y bitangent and camera-facing +Z. Normal scale is a catalog
parameter. Runtime mips must decode, average and renormalize normal vectors;
the script authors base levels only.

Sandstone uses irregular thick sediment, shale thin fractured laminae, and clay
rounded clumps. Organic materials use mottling; granular material coarse/fine
grain; rock cellular fractures; ore raised nodules; crystals irregular facets;
ice veins; metals hammered/brushed grain; liquids crossing ripples. Gases have
low-frequency mottling and zero normal scale. No animation or live fluid solver
is implied by these assets or initial-fill recipes.

Each background stack contains four original 512 x 512 RGBA layers: distant
opaque strata, cave silhouettes, middle formations and backwall details. The
latter three have transparent gaps. Parallax factors are 0.08/0.22/0.45/0.70.
The warm-grey Frontier and violet Hollows palettes and contrasting bands support
background-only refraction against a fixed world anchor.

There are 98 texture paths/usages. Base decoded RGBA bytes total 14,286,848
(90 x 128² x 4 plus 8 x 512² x 4). Complete ordinary mip chains total 19,049,000
bytes. These source-art sizes exclude GPU overhead and offscreen backdrop
memory and make no frame-performance claim.

## Permanent material ID ledger

The original seventeen IDs remain: basalt 16, sandstone 32, copper 49, clay 65,
iron 73, limestone 97, ice 128, salt 160, sulfur 193, masonry 257, crystal 385,
obsidian 509, water 1001, oxygen 1009, sand 1013, snow 1019, pebbles 1031.
Copper/iron display as native metals; crystal displays as amethyst.

New terrain: granite 600, shale 601, dirt 602, algae 603, slime 604, coal 605,
rust 606, phosphorite 607, aluminum 608, wolframite 609, diamond 610,
clear_quartz 611, glass 612, polluted_ice 613, iron_ore 614, copper_ore 615.
New granular: regolith 1100, ash 1101. New liquids: polluted_water 1200,
brine 1201, crude_oil 1202, magma 1203. New gases: polluted_oxygen 1300,
carbon_dioxide 1301, hydrogen 1302, chlorine 1303, steam 1304, natural_gas 1305.
All IDs are unique. Every material has a shipped composition, deposit,
template, boundary or fill reference.

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
The final pack identity is `c70c6df6a40ffaaf`. Release CPU previews passed for
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
