# lib/engine/mesh/

## Responsibility

`engine.mesh` owns CPU-side static mesh assets (static geometry + PBR factors + texture refs;
STATIC glTF/GLB only in MVP — skins/animations, OBJ/FBX deferred). P1 vocabulary + conversion
per `docs/plans/asset-pipeline.md` §§2.1/2.3/2.5/3 live here (`:types` + `:convert`); GPU
`TriangleBagCache` buckets live in `engine.app` passes (vertex-type-agnostic, static only).

## Design

- Partitioned umbrella: `Mesh.cppm` re-exports `:types` + `:convert` only (`:validate` folds
  into `:convert`).
- `:types` holds the frozen §2.3 vocabulary: `StaticMeshVertex` (60 B), `EMeshAttribute`
  (no joints/weights), `MeshPrimitiveRange` (stored base honoured), `StaticMeshAsset`,
  `UvTransformAsset`, `MaterialImageSlot::enabled()`, `MaterialAsset` (split MR maps,
  emissive map, occlusion/normal scalars, opaque/mask only), `ImageRef` (local duplicate —
  no mesh→image dependency), `SceneNodeAsset`/`SceneInstance`/`SceneAsset`, typed index IDs
  (`ImageAssetId`/`MaterialAssetId`/`MeshAssetId`/`NodeId`/`UvSetId` + invalid sentinels),
  and `mesh::errc` + `make_error_code`.
- `:convert` (`Mesh.Convert.cppm` decl + `Mesh.Convert.cpp` impl) is the synchronous
  thread-safe `importAndConvert(dir, file)`: Mango `importScene` per call, verbatim LH copy
  (tangent `w` kept verbatim pending the P1-parallel lighting probe), file-order indices +
  base honour, V-down no-flip, row-vector `S*R*T` / `parent*local`, MR split
  (roughness=g/metallic=b Linear), strip/fan expanded to lists mirroring Mango winding.
  Rejections: silent-empty→`invalid_argument`, missing POSITION/bad indices→`invalid_argument`,
  joints/skins→`function_not_supported`, blend + authored clearcoat/sheen/anisotropy/opacity→
  `function_not_supported`, animations→static snapshot with warning, Mango exceptions→
  `import_failed`. Embed bytes cloned via per-job `UniqueBuffer`→`moveToShared` (materialize
  propagated + `isMaterialized` checked); file refs via `SharedBuffer::mapFile`.
  Mango headers stay in the `.cpp` global fragment (PRIVATE dep, never exported).

## Flow

P1: file in → `SceneAsset` (meshes + materials + image refs + nodes + instances) out; app-side
pack joins `ImageRef` → `decodeTo_*`, uploads per mesh, packs per material (P2/P3).

## Integration

- Depends on: `engine.core` + `engine.math` (public), `mango-import3d` (private).
- Consumed by: (future) `engine.app` passes; `engine.tests.asset` (P1, Lane A owned).
- Build: `Mesh.cppm`, `Mesh.Types.cppm`, `Mesh.Convert.cppm`, `Mesh.Convert.cpp`
  (partition implementation unit) in `FILE_SET CXX_MODULES`;
  `setup_ppr_project(engine.mesh INTERNAL_PUBLIC_DEPS engine.core engine.math
  EXTERNAL_SYSTEM_PRIVATE_DEPS mango-import3d)`.

## Key Files

- `Mesh.cppm` — umbrella re-export only.
- `Mesh.Types.cppm` — `:types` frozen vocabulary + `mesh::errc`.
- `Mesh.Convert.cppm` — `:convert` `importAndConvert` declaration.
- `Mesh.Convert.cpp` — `:convert` implementation (only Mango-including TU).
- `CMakeLists.txt` — `engine.mesh` target registration (see Integration).
