---
name: procedural-pbr
description: Author editable, seeded procedural PBR materials in Blender, bake texture maps, and verify tiling and channel consistency. Use for offline material and texture asset generation.
---

# Procedural PBR materials

Use Blender's Python API and background Cycles baking for reproducible offline
materials. Discover the local Blender executable and version; do not assume an
MCP server or GPU is required. The supplied recipe was exercised on Blender
5.2.3 LTS with Cycles CPU on Linux; another host or version needs its own check.

Establish the requested surface, physical tile size, resolution, exposed controls,
seed, output destination, and consumer's channel conventions from task context.
Use reasonable defaults when these do not affect compatibility. Keep authoring
outputs outside a runtime asset tree until the user requests integration.

## Authoring and export

Build structure and masks first, then derive consistent PBR channels from them.
A chip, pore, crack, or coating should affect the relevant channels at the same
location. Preserve editable nodes and seed controls in a `.blend`. Avoid baked
lighting in base color. Use metallic values for material identity; intermediate
values normally belong only at filtered transitions or genuine mixed coverage.

For seamless UV materials, use periodic coordinates or periodic source nodes.
The reference maps UV onto a four-dimensional torus before noise sampling;
ordinary UV noise with Repeat sampling does not establish seamless generation.

Bake base color as sRGB and roughness, metallic, height, masks and normals as
Non-Color data. Bake scalar/color channels through emission to exclude lighting.
Save raw images, without a display/view transform. Export tangent normals with an
explicit convention; the reference uses +X, +Y, +Z (OpenGL). Verify the consumer's
convention rather than inferring it from camera handedness. Height needs a
documented physical range, UV scale, and any midpoint convention. Do not invent
AO by copying height. Bake AO only with an appropriate geometry/occlusion model.

Inspect PNG metadata as well as pixels: Blender's image save can leave sRGB/gamma
tags on Non-Color data. The reference strips those colorimetry chunks from data
maps and retains sRGB metadata on base color. A manifest alone cannot correct a
misleading file tag for other importers.

Use [scripts/painted_steel.py](scripts/painted_steel.py) as a runnable reference
recipe for chipped painted steel, not as a universal material generator. For a
new material, make a task-local recipe using the same export and QA principles;
retain only nodes and channels that serve that surface. The reference exports
six 16-bit RGB PNGs (scalar maps repeat the value in RGB), a manifest and an
editable `.blend`. PNGs are unpacked maps; engine channel packing is separate.

Example, substituting absolute script and output paths:

```sh
blender -b -t 8 --python-exit-code 1 --python /path/to/painted_steel.py -- --output /path/to/new-run --seed 17 --resolution 512 --previews
```

Use a fresh, empty output directory for each run. A nonzero script exit is a
failure; preserve its log and partial outputs. `--source /path/to/painted_steel.blend`
reopens the saved graph, changes its Seed value and rebakes it. This option is
for this recipe's generated files; arbitrary Blender files need explicit
material/object selection. Reopening clears scene objects to rebuild the bake
fixture, while retaining material edits. Save to a new output, never the source.
The selectable `AuthoringTile_25cm` uses the live procedural material; change
its Seed or shader inputs in Material Preview. It is hidden from proof renders.
Base color, roughness and metallic bakes follow the named Principled shader's
current input links/defaults. Height follows the named bump's Height input;
chip_mask remains the explicitly named structural mask. This is a single-layer
opaque metal/roughness recipe, not a baker for arbitrary layered BSDFs.

## Verification and delivery

For a new recipe or changed exporter, run the same seed twice in fresh processes
and a different seed once. Check decoded pixels, not just PNG file hashes. Check
dimensions, bit depth, finite/ranged channels, coherent masks, and a material
change when the seed changes. Determinism is scoped to the tested toolchain and
device; do not claim cross-platform pixel identity without evidence.

The reference's fixture-specific validator runs through Blender, using its
bundled NumPy, and exits nonzero on failure:

```sh
blender -b --python-exit-code 1 --python /path/to/verify_exports.py -- /path/to/first /path/to/repeat /path/to/variant --report /path/to/verification.json
```

The orientation and seam smoke checks are calibrated for this fixture at 512².
Lower resolutions can undersample its chip transitions and fail; a tiny bake is
useful for exporter checks but does not replace the 512² material trial.

It also checks scalar/color relationships, normal length and relief orientation,
and flags unusually large border jumps relative to interior gradients. First
and last pixel centers are adjacent periodic samples; they should not generally
be identical. Its seam thresholds are fixture smoke checks, not proof that an
arbitrary material is seamless. Adapt the channel contracts for other recipes.

Inspect a 2x2 tiled plane and sphere under at least two light directions,
reconstructing their material exclusively from exported maps. Check seams,
normal direction, scale, highlights, and absence of painted-in shadows. The
sphere is a look-development aid with UV distortion, not a scale measurement.
Reference previews show the normal-mapped surface without height displacement.

Deliver previews, the editable graph, maps, manifest and validation evidence.
State tested host/toolchain and any untested consumer integration. Do not label
the reference fixture a production material library or claim engine compatibility
until the actual import and render path has been exercised.
