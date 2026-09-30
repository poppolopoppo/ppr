# lib/engine/mesh/

## Responsibility

`engine.mesh` owns CPU-side static mesh assets (static geometry + PBR factors + texture refs;
STATIC glTF/GLB only in MVP — skins/animations, OBJ/FBX deferred). P1 vocabulary + conversion
per `docs/plans/asset-pipeline.md` §§2.1/2.3/2.5/3 live here (`:types` + `:convert`); GPU
`TriangleBagCache` buckets live in `engine.app` passes (vertex-type-agnostic, static only).

## Design

- Partitioned umbrella: `Mesh.cppm` re-exports `:types` + `:convert` only (`:validate` folds
  into `:convert`).
- `:types` holds the frozen §2.3 vocabulary: `StaticMeshVertex` (64 B plain-float
  arrays), `EMeshAttribute`
  (no joints/weights), `MeshPrimitiveRange` (stored base honoured), `StaticMeshAsset`,
  `UvTransformAsset`, `MaterialImageSlot::enabled()`, `MaterialAsset` (split MR maps —
  roughness=`.g`/metallic=`.b` linear — emissive map, occlusion/normal scalars, `EAlphaMode`
  opaque/mask only with blend rejected), `ImageRef` (local duplicate —
  no mesh→image dependency), `SceneNodeAsset` (`math::Transform` `m_local`/`m_world` + parent),
  `SceneInstance`/`SceneAsset`, typed index IDs
  (`ImageAssetId`/`MaterialAssetId`/`MeshAssetId`/`NodeId`/`UvSetId` + `none_v` sentinels),
  `MeshLimits`/`kDefaultMeshLimits` (GLB container + scene/allocation caps, over-limit fail-closed
  with `invalid_argument`), and `mesh::errc` + `make_error_code`. The error-category instance is
  namespace-scope `const MeshErrorCategory g_mesh_error_category{}` (`Mesh.Convert.cpp`) —
  `std::error_category` is polymorphic/non-literal, so never `constexpr` (same non-literal pattern
  fixed in `Shader.cpp`/`RHI.cpp`).
- `:convert` (`Mesh.Convert.cppm` decl + `Mesh.Convert.cpp` impl) is the synchronous
  thread-safe `importAndConvert(dir, file, limits = kDefaultMeshLimits)`: Mango `importScene` per call
  (GLB embeds re-resolved from PPR's own `mapFile` mapping via `resolveGlbEmbeds`; file refs mapped
  once per convert with `file_cache` dedupe by resolved path), verbatim LH copy
  (file-tangent `w` negated, missing tangents via in-fork MikkTSpace regen gated on the
  material normal map), file-order indices + base honour, V-down no-flip, MR split
  (roughness=`.g`/metallic=`.b` linear), strip/fan expanded to lists mirroring Mango winding.
  Nodes become `math::Transform`s (`fromMatrix` per node, worlds via `multiply(local, parentWorld)`
  BFS from parentless roots — unreached cyclic nodes fail closed; skinned nodes reject).
  One shared non-identity UV transform bakes into texcoords (`resolveBakedUvTransform`/
  `bakeUvTransform`, slots reset); non-uniform node scales bake per instance (positions by the
  world matrix, normals/tangents by its inverse-transpose + `orthonormalize`, node world reset
  to identity).
- Draw-range base convention (`MeshPrimitiveRange`): append-path primitives carry
  base=0 with remapped global indices, direct-glTF primitives carry
  base=vert_count with file-local indices; the resolved index is always
  (index + base), so both conventions draw correctly. File tangents are stored
  with negated `w` (the fork mirrors z on N/T, flipping bitangent handedness);
  MikkTSpace regens (missing TANGENT only) pass through verbatim as computed
  post-flip.
  Rejections: silent-empty→`invalid_argument`, missing POSITION/bad indices→`invalid_argument`,
  joints/skins→`function_not_supported`, blend + authored clearcoat/sheen/anisotropy/opacity→
  `function_not_supported`, animations→static snapshot with warning, Mango exceptions→
  `import_failed`. GLB embeds re-resolve from PPR's own `mapFile` mapping (`resolveGlbEmbeds`;
  mango blob views dangle — never read, unresolvable embeds fail closed); file refs map once
  per convert (`file_cache` dedupe by resolved path). Mango `Verbose` import chatter is
  silenced once at entry (`printEnable`, Error and above stay visible; `std::call_once` serializes the
  single process-global write so parallel imports never race restores). Mango headers stay
  in the `.cpp` global fragment (PRIVATE dep, never exported).

## Flow

P1: file in → `SceneAsset` (meshes + materials + image refs + nodes + instances) out; app-side
pack joins `ImageRef` → `decodeTo_*`, uploads per mesh, packs per material (P2/P3).

## Integration

- Depends on: `engine.core` + `engine.math` (public), `mango-import3d` (private).
- Consumed by: `engine.app` pass caches + scene ownership; `engine.tests.asset` (mesh
  import/convert/material/rejection/layout tests, GLB-embed + external-URI fixtures).
- Build: `Mesh.cppm`, `Mesh.Types.cppm`, `Mesh.Convert.cppm` in `FILE_SET CXX_MODULES`;
  `Mesh.Convert.cpp` is a PRIVATE source (the only Mango-including TU);
  `setup_ppr_project(engine.mesh INTERNAL_PUBLIC_DEPS engine.core engine.math
  EXTERNAL_SYSTEM_PRIVATE_DEPS mango-import3d)`.

## Key Files

- `Mesh.cppm` — umbrella re-export only.
- `Mesh.Types.cppm` — `:types` frozen vocabulary + `mesh::errc`.
- `Mesh.Convert.cppm` — `:convert` `importAndConvert` declaration.
- `Mesh.Convert.cpp` — `:convert` implementation (only Mango-including TU).
- `CMakeLists.txt` — `engine.mesh` target registration (see Integration).
