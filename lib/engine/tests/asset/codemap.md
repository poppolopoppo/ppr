# lib/engine/tests/asset/

## Responsibility

`engine.tests.asset` — asset-pipeline suite (`EngineAssetUnitTests`, `asset/` root).
Lane A owns the scaffolding plus the `asset/image` group: RGBA/block decode, format
predicates, block geometry, content-hash, errc mapping, frozen cross-thread sharing
(runtime-generated PNG/JPG/DDS fixtures; no binaries committed). Mesh/GPU-cache groups
arrive in later lanes without touching the image group.

## Design

- Same extern-umbrella + `Tests()`-accessor + POST_BUILD fixture-staging pattern as the
  core/app suites (MSVC C1001 workaround): `Asset.Tests.cppm` declares
  `extern const UnitTest asset`; `Asset.Tests.cpp` defines it and recurses the group
  accessors; groups are private `module engine.tests.asset;` TUs (`detail::` leaves +
  TU-local root + accessor); `main.cpp` runs `parseCli`/`runSuite`.
- Links `engine.image + engine.tests` (private `mango-image` for fixture encoding only);
  test dependencies never change the production graph.

## Flow

`main` → `runSuite(cli, asset)` → `imageTests()` (+ later groups) → per-leaf asserts
with `PPR_TEST_ASSERT` (engine asserts route to the runner policy, never abort).

## Integration

- Depends on: `engine.image`, `engine.tests` (public), `mango-image` (private).
- Staging: POST_BUILD `copy_directory assets/textures` → `<exe>/textures` (plus `meshes/`
  once fixtures exist); CTest `EngineAssetUnitTests` runs in the binary dir.

## Key Files

- `Asset.Tests.cppm` — extern umbrella decl only.
- `Asset.Tests.cpp` — `asset` root definition + group recursion.
- `Asset.Image.Tests.cpp` — `asset/image` group (decode/format/block/hash/error paths).
- `Asset.Staging.Tests.cpp` — `asset/staging` group (path-join separator regression).
- `main.cpp` — runner entry.
- `CMakeLists.txt` — target + CTest registration + staging.