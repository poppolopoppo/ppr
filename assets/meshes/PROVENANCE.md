# assets/meshes/ — fixture provenance

MVP mesh fixtures for `engine.mesh:convert` and the `engine.tests.asset` suite
(docs/plans/asset-pipeline.md §8). Each fixture is <100 KB, CC0, and exercises
one image-source path through `importAndConvert`:

- `textured_quad.glb` — 1 quad (POSITION + NORMAL + TEXCOORD), base-color PNG
  embedded in the GLB buffer view. Covers the embed path: Mango non-owning
  `ConstMemory` view → converter clones via `UniqueBuffer::clone` →
  `moveToShared` while the Mango `Scene` lives.
- `textured_box.gltf` + `textured_box.bin` + `textured_box.png` — external-URI
  path: geometry in `.bin`, texture in `.png`, both referenced by relative URI.
  Covers the file path: `ImageRef` records `m_rel_path` and the converter maps
  it with `SharedBuffer::mapFile`.
- Phase 8 M2 fixtures (hand-authored JSON reusing `textured_box.bin` /
  `textured_box.png` unless noted; same CC0 grant as above):
  - `uv_transform_box.gltf` — `KHR_texture_transform` (offset/rotation/scale)
    on the base-color texture; the convert bakes it into `m_texcoord`.
  - `second_set_box.gltf` — `TEXCOORD_1` accessor aliasing the set-0 data with
    `texCoord: 1` on the base-color slot; the selector flows convert into pack.
  - `mixed_set_box.gltf` — identity transforms on sets 0 (base color) and 1
    (normal); converts cleanly with per-slot selectors preserved.
  - `divergent_transform_box.gltf` — conflicting non-identity transforms across
    slots; the convert rejects with `function_not_supported`.
  - `transformed_set_box.gltf` — non-identity transform on set 1; rejected with
    `function_not_supported` (no channel to bake into).
  - `degenerate_node_box.gltf` — zero node scale; rejected with
    `invalid_argument` (singular world has no inverse-transpose).
  - `scaled_twin_tri.gltf` + `scaled_twin_tri.bin` — one triangle (diagonal
    normals, mixed zero/non-zero normals/tangents) instanced by an identity
    node and a node scaled `[2.0, 0.5, 1.0]`; the scaled instance bakes via
    inverse-transpose. `mirror_twin_tri.gltf` reuses the same `.bin` with a
    `[-2.0, 0.5, 1.0]` node to prove the tangent-w mirror flip.

## License

All files in this directory are released under CC0 1.0 Universal (public
domain dedication), suitable as freely redistributable test data. If a fixture
is regenerated or replaced, record its source, author, and license grant here.

## Staging

Fixture binaries are staged onto the `engine.tests.asset` target by Lane A
(`lib/engine/tests/asset/CMakeLists.txt` POST_BUILD); runtime resolves them
via `getContentDir()/"meshes"`. This file only records provenance.
