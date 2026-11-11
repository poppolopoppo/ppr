# assets/

## Responsibility

Runtime asset root for the demo. Shader sources that `engine.shader` compiles at startup and `engine.rhi` links into pipelines live in `shaders/`; `textures/` holds shared runtime textures; `meshes/` contains static GLB fixtures, including 16 staged Kenney colony assets under `meshes/kenney_colony/`; `fonts/` stages pinned UI font binaries and license notices (no binaries committed yet); `lighting/` holds the generated calibrated thermal lookup table and its regeneration metadata. Shader, texture, mesh, font, and lighting trees stage beside executables on every explicit consumer build.

## Design

- `meshes/kenney_colony/` preserves the upstream pack-relative `Models/<format>/` layout, required texture siblings, per-pack licenses, and `PROVENANCE.md`; runtime code resolves it only below `getContentDir() / "meshes"`.
- `game/CMakeLists.txt` synchronizes `assets/shaders`, `assets/textures`, `assets/lighting`, and (when present) `assets/meshes` and `assets/fonts` beside the executable; runtime never reads these trees from the source checkout.
- `lighting/thermal_lut.bin` is the committed 2,048-row, 16-byte-stride calibrated thermal-emission table; `thermal_lut.json` pins its source CIE checksum/license, binary64 integration constants and table SHA-256. `scripts/generate_thermal_lut.py --check` verifies both offline; startup never downloads data or regenerates the table.
- ImGui overlay shader is NOT an asset — it is an embedded source string in `lib/engine/app/ui/App.UI.ImGui.cpp`.

## Flow

`game/CMakeLists.txt` synchronizes the shader, texture, mesh, font, and lighting trees beside the executable. At startup `IShaderService::loadModuleFromFile` compiles the shaders, then the game fixture imports `meshes/kenney_colony/*.glb`, decodes referenced images, and uploads the scenes to GPU caches.

## Integration

- **Consumers**: `engine.shader` (compilation), `engine.rhi` / `Renderer` + `TrianglePass` (pipelines), `game` demo.
- **Depends on**: Slang toolchain (compile-time of the asset at runtime).
- **Provides**: on-disk shader sources; deployed copy next to `app.game`.

## Key Files

- `shaders/` — Slang sources (see `assets/shaders/codemap.md`).
- `textures/` — shared runtime textures (see `textures/PROVENANCE.md`; no binary fixtures committed yet).
- `meshes/` — static GLB/gltf fixtures, including 16 staged Kenney colony assets under `meshes/kenney_colony/`.
- `fonts/` — pinned UI font binaries and license notices (see `fonts/PROVENANCE.md`; no binaries committed yet).
- `lighting/` — generated calibrated thermal lookup (`thermal_lut.bin`, `thermal_lut.json`, 32 KiB, stride 16); source provenance is `third_party/CIE/README.md`.

`worldgen/default/pack.toml` is the schema-1 `ppr_frontier` named descriptor
manifest for elements, materials, fields, biomes, features, rules, backgrounds,
worlds and textures. Texture descriptors own their bitmap references;
`game/CMakeLists.txt` stages this tree on every explicit game/test build,
including data-only edits/deletions. CPU `--validate-worldgen` validates deployed
definitions without preparing texture payloads.

The explicit playable `lighting_showcase` selects its own muted stone backwall
stack and four generated textures, reusing the current procedural generation
facility. Frontier/hollows are unchanged. Background radiance still comes from
the native world field, with no unlit fill; the separately labelled cut-face
presentation experiment is confined to the native foreground shader. See pack
`PROVENANCE.md` for generator command and matched-capture controls.


The showcase service bay adds four repeat-sampled manufactured materials and
eight generator-owned128x128 albedo/normal PNGs. Housing/rivets, raised tread and
oriented pipe/union relief use neutral pigment and matching analytic height;
there is no baked lighting. They are actual Grid terrain/collision labels and
consume the existing world field, not fixed-lit mesh decoration. See pack
`PROVENANCE.md` for the exact limited generator command and source boundaries.

The authored `lighting_lab` world record adds eleven ordinary material records
(wall/red/green/emitter/glass/hot-metal/PBR0..3/magma), a white unit albedo and
flat-normal texture pair, and the fixed-room world palette. All are normal pack
assets with catalog-assigned numeric IDs; there is no second lighting palette
or renderer-only content branch. The pack manifest remains the single shipped
list, now within its 96 KiB manifest bound.

The two pipe normal maps use physical world slopes for radius=half the texture
period, with the existing full height field and 128² sample phase preserved.
Only those two keys use `(size/2)*(.5/.30)` central-difference scaling; other
materials retain their existing factor. The accepted incremental correction
preserves pipe albedos, descriptors, geometry and optical/source data. Its
silhouette cusp is not a smooth-normal seam; full environment/art/performance
acceptance remains separate. See [pipe normal provenance](worldgen/default/PROVENANCE.md#physical-pipe-normal-correction).

The shared ordinary-world landing paints a four-cell ceiling-mounted amber
source at local native x44..45/y31..32, directly below the unchanged masonry
ceiling y33..35. Frontier and Hollows reference the existing pack material
`showcase_amber_lamp` as numeric ID64, matching the showcase palette; there is no
new pack entry, material, texture or emission setting. The source is actual
opaque Grid terrain, so its colliders and immutable optical capture share the
same authored cells. The spawn/resource markers, floor, room walls, reservation
and existing background stacks remain unchanged. Black-sky native lighting must
illuminate the receiving backwall through the open lower/side source faces;
visual acceptance and the full-world resource/performance gates remain separate.
