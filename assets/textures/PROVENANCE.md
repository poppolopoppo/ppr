# assets/textures/ — Provenance

All texture fixtures staged here are CC0 or equivalently license-free, under
100 KB each, per `docs/plans/asset-pipeline.md` §8.

## Runtime-generated fixtures (P1, Lane A)

`engine.tests.asset` generates its image fixtures at runtime (Mango-encoded
PNG/JPG + a hand-built 4x4 DXT1 DDS) into `temp_image_fixtures/` under the test
working directory. No binary textures are committed yet, so this directory
currently stages only this file alongside the test/demo executables.

## Planned committed fixtures (Lane B and later)

- `textured_quad.glb` (GLB-embed path) + `textured_box.gltf + .bin + .png`
  (external-URI path): source, author, and CC0 statement recorded here on
  arrival. Fixture staging (`assets/textures -> <exe>/textures`,
  `assets/meshes -> <exe>/meshes`) is already wired in `game/CMakeLists.txt`
  and `lib/engine/tests/asset/CMakeLists.txt`.
