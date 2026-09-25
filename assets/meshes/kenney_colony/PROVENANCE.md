# Kenney colony fixture

This directory contains temporary, committed static content sourced from
**Kenney Game Assets All-in-1 3.5.0**, `3D assets`. Runtime resolution starts at
`getContentDir() / "meshes" / "kenney_colony"` and never reads the original `D:`
source path.

The fixture is a compact cutaway side-view colony habitat composed around the
editor's default camera at `(0, 0, -2)` looking toward positive Z. The visual
concept is **Kepler-9 Cutaway Waystation**: a three-module shell and central gate
anchor the silhouette; stairs, a catwalk, and a ladder establish vertical
circulation; storage and production occupy the left/center; the bed, table, and
food establish the domestic right side. The layout intentionally stays within
roughly four world units so it fits the inherited establishing shot without a new
camera API.

## Runtime-verified static GLBs (16)

- Space Station Kit / Models / GLB format (10): `floor.glb`, `wall.glb`,
  `wall-window.glb`, `wall-door-center.glb`, `door-single.glb`, `stairs.glb`,
  `pipe.glb`, `bed-single.glb`, `container-tall.glb`, `table.glb`
- Factory Kit / Models / GLB format (4): `catwalk-straight.glb`, `machine.glb`,
  `conveyor.glb`, `pipe-large-valve.glb`
- Prototype Kit / Models / GLB format (1): `ladder.glb`
- Food Kit / Models / GLB format (1): `bread.glb`

A production-path runtime probe imported all 16 GLBs, resolved and decoded every
image reference, uploaded all 16 scene receipts, and submitted 21 active
placements / 22 primitive draws. Four 128×64 texture siblings are shared by the
texture cache across pack uploads; no mip chain is generated.

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

- **Shell (7 placements):** three floor modules and a three-piece rear wall with
  window, doorway header, and inset door create the readable cutaway silhouette.
- **Circulation (4):** stairs at the left transition, two catwalk modules across
  the upper plane, and a ladder near the gate create a legible route into the
  main room.
- **Services/storage (7):** a tall container, machine, conveyor, valve, and three
  overhead pipe modules form a dense but separated utility band.
- **Habitat/domestic (3):** bed, table, and bread form a right-side living area;
  the bread rests on the table rather than floating.

All transforms use row-major row-vector multiplication, the shared positive
uniform scale `1.0`, and deliberate front-plane offsets. No random rotations are
used.

`ApplicationEditor` exposes no safe startup camera setter: the inherited editor
fixes its controller at `(0, 0, -2)` looking toward positive Z. The compact
layout is therefore composed around that default establishing shot rather than
adding a camera API.

## Runtime validation status

The D3D12 null-SRV validation spam was traced to `TrianglePass` binding
`g_textures` only on the first shader object in each render invocation. Every
`bindPipeline` call creates a new object-local root shader object, so later draws
retained a null `g_textures` SRV. The direct, CPU-indirect, and compute-indirect
paths now bind the shared descriptor buffer on every root object.

A post-fix 15-second debug run completed 16 loads, 21 placements, 22 draws, and
16 reverse releases with zero D3D12 GPU-validation messages. A preview capture
is still visually inconclusive (white client), leaving camera/presentation or
content visibility as a separate actionable follow-up rather than a descriptor
validation failure.

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
