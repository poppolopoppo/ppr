# lib/engine/tests/asset/

## Responsibility

`engine.tests.asset` — asset-pipeline suite (`EngineAssetUnitTests`, `asset/` root):
`asset/image` (RGBA/block decode, format predicates, block geometry, content-hash, errc
mapping, frozen cross-thread sharing via runtime-generated PNG/JPG/DDS fixtures — no
binaries committed), `asset/mesh` (GLB-embed + external-URI import, verbatim LH values,
MR split, rejections, malformed-GLB fail-closed, 64 B layout, ID sentinels),
`asset/staging` (path-join separator regression), `asset/gpu` (handles, material pack,
pipeline variants, bindless budget, upload/resolve/release), `asset/gate` (textured
render gate + tangent arbitration + editor flow via TestApp boot + readback).

## Design

- Same extern-umbrella + `Tests()`-accessor + POST_BUILD fixture-staging pattern as the
  core/app suites (MSVC C1001 workaround): `Asset.Tests.cppm` declares
  `extern const UnitTest asset`; `Asset.Tests.cpp` defines it and recurses the group
  accessors; groups are private `module engine.tests.asset;` TUs (`detail::` leaves +
  TU-local root + accessor); `main.cpp` runs `parseCli`/`runSuite`.
- Links `engine.image + engine.mesh + engine.app + engine.rhi + engine.shader +
  engine.core + engine.math + engine.tests` (private `mango-image` for fixture encoding,
  private GLFW for GPU integration); test dependencies never change the production graph.

## Flow

`main` → `runSuite(cli, asset)` → group accessors → per-leaf asserts
with `PPR_TEST_ASSERT` (engine asserts route to the runner policy, never abort).

## Integration

- Depends on: `engine.image`, `engine.mesh`, `engine.app`, `engine.rhi`, `engine.shader`,
  `engine.core`, `engine.math`, `engine.tests`.
- Staging: POST_BUILD `copy_directory assets/textures`, `assets/meshes`, `assets/shaders`
  beside the binary; CTest `EngineAssetUnitTests` runs in the binary dir.

## Key Files

- `Asset.Tests.cppm` — extern umbrella decl only.
- `Asset.Tests.cpp` — `asset` root definition + group recursion.
- `Asset.Image.Tests.cpp` — `asset/image` group (decode/format/block/hash/error paths).
- `Asset.Search.Tests.cpp` — `asset/mesh` group (import/convert/material/rejections/layout).
- `Asset.Staging.Tests.cpp` — `asset/staging` group (path-join separator regression).
- `Asset.Gpu.Tests.cpp` — `asset/gpu` group (handles/pack/variants/budget/caches).
- `Asset.Gate.Tests.cpp` — `asset/gate` group (render gate, arbitration, editor flow).
- `main.cpp` — runner entry.
- `CMakeLists.txt` — target + CTest registration + staging.