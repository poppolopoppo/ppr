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

## License

All files in this directory are released under CC0 1.0 Universal (public
domain dedication), suitable as freely redistributable test data. If a fixture
is regenerated or replaced, record its source, author, and license grant here.

## Staging

Fixture binaries are staged onto the `engine.tests.asset` target by Lane A
(`lib/engine/tests/asset/CMakeLists.txt` POST_BUILD); runtime resolves them
via `getContentDir()/"meshes"`. This file only records provenance.
