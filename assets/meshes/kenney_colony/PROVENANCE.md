# Kenney colony fixture

This directory contains temporary, committed static content sourced from
**Kenney Game Assets All-in-1 3.5.0**, `3D assets`. Runtime resolution starts at
`getContentDir() / "meshes" / "kenney_colony"` and never reads the original `D:`
source path.

The fixture is a measured cutaway side-view colony habitat composed around the
editor's default camera at `(0, 0, -2)` looking toward positive Z. The visual
concept is **Kepler-9 Cutaway Waystation**: a roughly 20-unit-wide shell
establishes the full silhouette while the entrance, stairs, ladder, catwalk, and
raised domestic room stay concentrated near the inherited central view. Services
and storage occupy the outer bands; three deliberate Z layers separate foreground
traffic, the central room, and the background shell without a new camera API.

## Runtime-verified static GLBs (16)

- Space Station Kit / Models / GLB format (10): `floor.glb`, `wall.glb`,
  `wall-window.glb`, `wall-door-center.glb`, `door-single.glb`, `stairs.glb`,
  `pipe.glb`, `bed-single.glb`, `container-tall.glb`, `table.glb`
- Factory Kit / Models / GLB format (4): `catwalk-straight.glb`, `machine.glb`,
  `conveyor.glb`, `pipe-large-valve.glb`
- Prototype Kit / Models / GLB format (1): `ladder.glb`
- Food Kit / Models / GLB format (1): `bread.glb`

A production-path runtime probe imported all 16 GLBs, resolved and decoded every
image reference, uploaded all 16 scene receipts, and submitted 53 active
placements / 55 primitive draws. Four 128×64 texture siblings are shared by the
texture cache across pack uploads; no mip chain is generated.

## Measured source bounds and anchors

The following values were read from every staged GLB's transformed scene bounds
and are also emitted by the production-path `colony source bounds` log after
PPR mesh conversion. Bounds are `(X, Y, Z)` in source world units. Except for
the two noted pieces, the source anchor is the local `Y=0` base.

| Asset | Size | Source anchor / orientation evidence |
|---|---:|---|
| `catwalk-straight.glb` | `(1.000, 0.601, 1.255)` | Y spans `-0.147..0.454`; long axis is Z, so side-catwalk placements use a quarter turn. |
| `conveyor.glb` | `(1.000, 0.400, 1.000)` | Y `0..0.400`; long axis is X. |
| `machine.glb` | `(1.200, 1.300, 1.500)` | Y `0..1.300`; freestanding base. |
| `pipe-large-valve.glb` | `(1.000, 1.000, 1.016)` | Y `0..1.000`; upright service part. |
| `bread.glb` | `(0.442, 0.040, 0.366)` | Y `0..0.040`; rests on the measured table top. |
| `ladder.glb` | `(0.150, 1.000, 0.550)` | Y `0..1.000`; upright along Y. |
| `bed-single.glb` | `(0.500, 0.350, 1.000)` | Y `0..0.350`; long axis is Z. |
| `container-tall.glb` | `(0.800, 0.900, 0.800)` | Y `0..0.900`; upright storage volume. |
| `door-single.glb` | `(0.400, 0.700, 0.100)` | Y `0..0.700`; upright in the XY entrance plane. |
| `floor.glb` | `(1.000, 0.300, 1.000)` | Y `0..0.300`; X/Z tile module. |
| `pipe.glb` | `(0.246, 0.500, 0.285)` | Y `0..0.500`; upright utility run. |
| `stairs.glb` | `(1.000, 0.300, 1.000)` | Y `0..0.300`; four Y bands rise along local X, so repeated stairs advance +X. |
| `table.glb` | `(1.100, 0.400, 0.600)` | Y `0..0.400`; long axis is X. |
| `wall-door-center.glb` | `(1.000, 0.300, 0.300)` | Y `0.700..1.000`; this is a header/lintel, not a full wall. |
| `wall-window.glb` | `(1.000, 1.000, 0.300)` | Y `0..1.000`; upright in the XY wall plane. |
| `wall.glb` | `(1.000, 1.000, 0.300)` | Y `0..1.000`; upright in the XY wall plane. |

## Texture normalization

The upstream Space Station, Factory, Prototype, and Food `Textures/colormap.png`
files are 512×512 palette PNGs. The current developer decode path asserts when
its 32,768-byte `LocalCache` block receives the 1,048,576-byte RGBA8 job required
by a 512×512 source. The staged siblings were therefore resized once, without
changing their filenames or GLB-relative references, to 128×64 RGBA8 PNGs. Their
decoded storage is exactly 32,768 bytes and preserves normalized UV content.
This is a fixture-only asset adaptation; no engine code was changed.

Upstream `License.txt` files are retained for all four staged packs. Pack-relative
`Models/GLB format/` and `Textures/` layout is preserved.

## Scene composition

- **Shell (27 placements):** ten scale-2 rear wall/window modules form a measured
  20-unit shell; six side modules close the cutaway; a header/door pair establishes
  the entrance; nine floor modules establish the inhabited platform.
- **Circulation (12):** four measured stair repeats rise along +X, a ladder reaches
  the upper floor, four quarter-turned catwalk modules run across X, and three
  raised floor modules form the habitat deck.
- **Services/storage (11):** foreground machine/conveyor traffic, midground
  conveyors/valve, two storage containers, and four background pipe runs form
  separated utility bands.
- **Habitat/domestic (3):** bed, table, and bread form the raised central living
  area; the bread is placed at the scaled table's measured top height.

All transforms use row-major row-vector multiplication, validated positive
per-placement uniform scale, and explicit quarter turns. No random rotations or
mirroring are used. The shell spans X `[-10, 10]`; active content uses three
foreground/midground/background Z bands.

`ApplicationEditor` exposes no safe startup camera setter: the inherited editor
fixes its controller at `(0, 0, -2)` looking toward positive Z. The full shell
establishes scale, while the entrance, circulation, and habitat are deliberately
kept near that view rather than adding a camera API.

## Runtime validation status

The D3D12 null-SRV validation spam was traced to `TrianglePass` binding
`g_textures` only on the first shader object in each render invocation. Every
`bindPipeline` call creates a new object-local root shader object, so later draws
retained a null `g_textures` SRV. The direct, CPU-indirect, and compute-indirect
paths now bind the shared descriptor buffer on every root object.

The editor now presents two explicit native passes for the same acquired surface
image: TrianglePass selects the renderer's resize-matched D32Float depth view;
ImGui then runs in a color-load pass with no depth attachment. The focused
`triangle_depth_occlusion_gate` proves the 3D pipeline rejects a farther fragment
drawn after the nearer fragment. The quarantined `editor_scene_flow` exercises
the real depth-enabled 3D pass followed by the no-depth ImGui pass. Both passed
three shuffled loops with no D3D12 GPU-validation errors. The debug layer did emit
a non-fatal ImGui font-atlas barrier-efficiency message during the UI path; it is
not a depth/pipeline validation error. A fresh 1296×759 debug-build window capture
showed the separated shell, services, circulation, and habitat bands with no white
or blank client.

## Omitted candidates and concrete reasons

- `Blocky Characters/.../character-a.glb` and `character-b.glb` contain 27
  animation clips each; `Cube Pets/.../animal-cat.glb` contains eight. They are
  prohibited animated assets and are not staged.
- Furniture `kitchenFridge.glb`, `kitchenStove.glb`, `washer.glb`, and
  `shower.glb` fail static import with `mesh content is deferred from MVP
  (skins, blend, or authored extension modules)`.
- Furniture `toilet.glb` and Nature `plant_bush.glb` import and upload but produce
  repeated D3D12 GPU validation errors for a null SRV while drawing. They are not
  staged in this runnable cache fixture.
- Nature accents and inhabitants/pet are therefore unavailable. The selected
  compact scene keeps its open background rather than filling it with unrelated
  substitutes.
- Unused compatible candidates were removed to keep the runtime at 16 source
  uploads: `floor-panel-straight.glb`, `stairs-handrail.glb`, `pipe-bend.glb`,
  `computer.glb`, `box-large.glb`, `hopper-square.glb`, `screen-wide.glb`,
  `carrot.glb`, and `rock_smallA.glb`.
- Optional `Prototype Kit/Models/GLB format/floor-square.glb` is not staged;
  the three selected floor modules form the stable base.

No FBX, OBJ, DAE, STL, SKP, BLEND, animated, or other non-static format is
staged.

## Rollback

Remove this directory and remove the colony loading, placement, submission, and
release members from `game/main.cpp`. The inherited editor scene flow is not
used by this fixture and remains independent.
