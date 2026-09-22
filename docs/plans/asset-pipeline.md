# Asset Pipeline — Deep Work Plan (draft, NOT approved)

> Status: PLAN ONLY — no implementation performed. NOT approved and NOT ready to execute
> until every gate below passes. Execution by a fresh session starts at Phase 0.
> Vision (XNA-style): an asset = path + importer + processor. Mango owns import/conversion,
> Slang-RHI owns bindless GPU resources, PPR glue stays minimal.
> Final gate: import a static textured glTF/GLB mesh via Mango and render it on screen with a new
> bindless shader through the refactored `TrianglePass`.
>
> Source-format law (AGENTS.md + precedent): types `PascalCase`; members `m_snake_case`
> (cf. `FrameConstants::m_view_projection/m_camera_position/m_viewport_size`,
> `App.Renderer.TrianglePass.cppm:24-30`); functions `lowerCamel`; enum values `snake_lowercase`
> (cf. `shader::errc::invalid_arg`, `Log::ELevel::debug`); file partition after colon maps to dotted
> suffix. Every struct below follows it — no camelCase members, no Pascal enum values.
> Errors: `Expected<T>` = `std::expected<T, std::error_code>` (`Core.Utility.cppm:83-84`) —
> single-arg spelling in every signature below. `import std` appears only in TUs that use it
> (per `module-architect`); never "every TU".
> This plan is self-contained: all gates, decisions, and details live here, not in any
> ignored `.slim/` or deepwork file.

## 0. Locked decisions (do not relitigate without a new oracle review)

1. MVP = textures first, meshes second, same importer→processor shape for both.
2. Synchronous but thread-safe APIs; async/worker dispatch lives one level up (future `engine.asset`
   ContentManager), never inside `engine.image` / `engine.mesh`.
3. New top-level modules `engine.image` (CPU textures) and `engine.mesh` (CPU meshes).
   `engine.asset` reserved for the future XNA orchestrator. Graph (§9):
   `engine.app → engine.image, engine.mesh`; `engine.image → engine.core + engine.math`
   (+ PRIVATE mango-image, never exported); `engine.mesh → engine.core + engine.math`
   (+ PRIVATE mango-import3d, never exported); never `rhi`/`app` from image/mesh.
4. Formats MVP: PNG/JPG/KTX2/DDS via Mango `Bitmap` for images; STATIC glTF/GLB only for meshes
   (static geometry + PBR factors + texture refs). DEFERRED to post-MVP: skins/animations
   playback AND storage, OBJ/FBX, and any material extension not represented in `MaterialAsset`
   (specular/transmission/volume/iridescence, emissive strength). The converter REJECTS or
   explicitly drops unrepresented content per §2.3 (never silently widens scope).
5. `Image` (CPU asset) vs `Texture` (GPU resource) are different types in different modules.
6. GPU caches are **pass-owned** (`TrianglePass`/future `MeshPass`), NOT Renderer-owned.
   Renderer stays content-free (AGENTS.md contract).
7. Handles are `SparseKeyId`-backed strong types (§2.1), usable only AFTER the P0 core fix.
   Caches named `*Cache` uniformly (never Factory/Registry). Raw u32/u64 indices are BANNED
   at API boundaries (§2.1 ID vocabulary).
8. `TriangleBagCache` is **vertex-type-agnostic**: one class, buckets keyed by layout identity;
   static geometry only in MVP (no skinned bucket exists yet; the skinned variant is a
   post-MVP addition that must never alias a static bucket or draw call).
9. NO `IService` singletons for image/mesh (stateless pure conversion needs no service;
   device state lives in passes). Only a future shared-mutable seam (cross-pass dedup cache,
   async ContentManager) may warrant one.
10. Storage rides the `engine.core:memory.buffer` abstractions (§2.5): `UniqueBuffer` while mutable,
    `SharedBuffer` once frozen; typed `Array`s stay for mesh verts/indices.

## 1. Canonical naming (XNA `*Asset` family — proposed, not yet ratified)

- Modules/partitions: `engine.image:types` + `engine.image:decode`; `engine.mesh:types` + `engine.mesh:convert`
  (`:validate` folds into `:convert`). Umbrellas re-export only, no logic.
- CPU assets: `ImageAsset`, `StaticMeshAsset`, `MaterialAsset`,
  `SceneAsset`, `MeshPrimitive` / `MeshPrimitiveRange` (Mango `Primitive` collides — never bare).
  Rejected: `Converted*`, `Generic*` (collides with `hal/generic/`), `Intermediate*`,
  triple-named image (single `ImageAsset` only). No `SkinnedMeshAsset` in MVP (deferred).
- `StaticMeshVertex{m_position, m_normal, m_texcoord, m_tangent, m_color}` — NO joint/weight fields.
- GPU: `BindlessTextureCache`, `TriangleBagCache`, `BindlessMaterialCache`, `GpuMaterial`,
  `GpuTextureRefs`, `GpuMaterialFlags`, `TriangleBagRange`, `TriangleBagHandle`. Handles
  `TextureHandle`/`MaterialHandle` scoped (never bare `Handle`).
- Params: `*Desc` (`ImageDecodeDesc`); errors: `image::errc` / `mesh::errc` + `make_error_code` +
  `std::is_error_code_enum` (the `shader::errc` pattern).
- Shader: `assets/shaders/mesh_bindless.slang`; `g_vertices/g_indices/g_materials/g_textures`;
  PascalCase structs with `m_` snake fields (mirrors `triangle.slang`/`g_frame`).
- Fixtures: `assets/textures/`, `assets/meshes/`; `textured_quad.glb` (embed),
  `textured_box.gltf + .bin + .png` (external URI); <100 KB, CC0 with provenance file
  (§8: `assets/meshes/PROVENANCE.md`, `assets/textures/PROVENANCE.md`).

## 2. Frozen contracts

### 2.0 P0 prerequisite — fix `SparseKeyId` default-valid bug in `engine.core` FIRST

Current fact (`Core.Containers.SparseVector.cppm:61-72`): default is
`m_index = 0xFFFFFFu, m_seed = 0xFFu`, and `isValid()` returns `m_seed != 0u` —
so a **default-constructed key is currently considered VALID**. That inverts the
documented default-invalid contract and every handle plan built on it.

Mandatory P0, before any handle use in this plan:

1. Change the default to invalid: `m_index = 0u`-family / `m_seed = 0u` (exact values
   owned by the core fix; the invariant is `SparseKeyId{}.isValid() == false`).
2. Add regression tests in `engine.tests.core`: default-constructed invalid,
   `add → valid`, `erase → stale fails closed`, seed-reuse validation, `<=>` ordering
   of invalid vs valid.
3. Only then introduce `Numeric<SparseKeyId, Tag>` handles below. No unsafe fallback
   (no plain-struct shim, no "treat 0xFFFFFF/0xFF as invalid at call sites") that would
   mask the core bug. If the core fix is rejected, this whole plan blocks.

### 2.1 Handles + index IDs — `SparseKeyId` / `Numeric` (no raw indices)

Post-P0 `SparseKeyId`: `u24 index + u8 seed`, default-invalid (seed 0),
generation-validated reuse, `==`/`<=>`/`hashValue` built-in. Caches store entries in
`SparseVector<T>` (`add → key`, `tryGet` checks generation, `erase` invalidates).
Precedent: `WindowHandle = Numeric<void*, Window>`. `Numeric` (`Core.Types.cppm:222`)
requires `equality_comparable + three_way_comparable`, is constexpr-constructible,
and provides `==`/`<=>`; free functions fill the validity gap:

```cpp
// engine.app:renderer.types (visible to passes)
struct TextureHandleTag {};  struct MaterialHandleTag {};  struct TriangleBagHandleTag {};
using TextureHandle      = Numeric<SparseKeyId, TextureHandleTag>;
using MaterialHandle     = Numeric<SparseKeyId, MaterialHandleTag>;
using TriangleBagHandle  = Numeric<SparseKeyId, TriangleBagHandleTag>;
[[nodiscard]] bool isValid(TextureHandle) noexcept;      // forwards to key.isValid()
[[nodiscard]] bool isValid(MaterialHandle) noexcept;
[[nodiscard]] bool isValid(TriangleBagHandle) noexcept;

// Index-ID vocabulary — every index into typed storage is a Numeric<u32, Tag> alias:
using ImageAssetId         = Numeric<u32, struct ImageAssetIdTag>;         // into SceneAsset::m_images
using MaterialAssetId      = Numeric<u32, struct MaterialAssetIdTag>;      // into SceneAsset::m_materials
using MeshAssetId          = Numeric<u32, struct MeshAssetIdTag>;          // into SceneAsset::m_meshes
using NodeId               = Numeric<u32, struct NodeIdTag>;               // into SceneAsset::m_nodes
using UvSetId              = Numeric<u32, struct UvSetIdTag>;              // TEXCOORD_n set selector
using BagBucketId          = Numeric<u32, struct BagBucketIdTag>;          // TriangleBagCache layout-bucket key
using TextureBindlessIndex = Numeric<u32, struct TextureBindlessIndexTag>; // slot in Texture2D[] (GPU protocol uint)
inline constexpr ImageAssetId kInvalidImage{0xFFFFFFFFu};        // typed none (asset-local indices have no seed)
inline constexpr MaterialAssetId kInvalidMaterial{0xFFFFFFFFu};  // typed none (no override)
inline constexpr NodeId kInvalidNode{0xFFFFFFFFu};               // typed none (root parent)
inline constexpr TextureBindlessIndex kNoTexture{0xFFFFFFFFu};   // GPU none-sentinel (shader compares to this)
```

P0 checks: `Numeric<SparseKeyId, Tag>` and `Numeric<u32, Tag>` must be constexpr-constructible,
equality-comparable, hashable; GPU-resident aliases additionally `standard_layout` + `sizeof == 4`
(`static_assert` each). Handles are indices (third ownership category beside RAII-owning and
ref/view/`safe_ptr` non-owning); pin-while-held refcount stays INTERNAL to pass-owned caches
(no shared ownership escapes, no globals). Strong aliases serialize to `u32` protocol fields
via explicit `*alias`/`u32(alias)` conversion at the boundary — never by `reinterpret_cast`.

### 2.2 `engine.image:types` — CPU only; NO mango/rhi types cross

`rhi::Format`/`SubresourceData` are slang-rhi re-exports — they must NOT appear here (would make
`image → rhi`, breaking the graph). Store a native format + plain layout; map to RHI at upload
inside `engine.app` caches. `ConstMemory` (mango) crosses only as `SharedBufferView`
(`mem::SharedBufferView = span<const byte>`); owning bytes ride `SharedBuffer` (§2.5):

```cpp
enum class BlockTag : u32 { none, bc1, bc3, bc4, bc5, bc7, astc4x4, astc6x6, astc8x8 };
enum class NativeImageFormat : u32 { rgba8_linear, rgba8_srgb, bc1_linear, bc1_srgb, bc3_linear,
  bc3_srgb, bc4_linear, bc5_linear, bc7_linear, bc7_srgb, astc4x4_linear, astc4x4_srgb,
  astc6x6_linear, astc6x6_srgb, astc8x8_linear, astc8x8_srgb };
enum class ImageUsage : u8 { color, data };
enum class ImageDimension : u8 { image2d };
struct ImageDecodeDesc { bool m_simd = true; bool m_multithread = false; bool m_flip_v = false; };
// multithread=false is engine POLICY (no Mango pool in RT path), not the Mango default.
// flip_v=false: glTF/Mango V already matches (import_gltf.cpp:740-742).
struct ImageSubresource {
  mem::SharedBuffer m_view;               // subspan() of m_storage; pins the outer (Buffer.cpp:276-288)
  u64 m_row_pitch = 0, m_slice_pitch = 0; // POSITIVE, sampler order (never negative-stride views)
};
struct ImageAsset {
  u32 m_width = 0, m_height = 0, m_mip_count = 1;
  ImageDimension m_dimension = ImageDimension::image2d;
  NativeImageFormat m_format = NativeImageFormat::rgba8_linear;
  BlockTag m_tag = BlockTag::none;
  u32 m_block_w = 1, m_block_h = 1, m_bytes_per_block = 4;
  bool m_is_srgb = false, m_is_block = false;
  mem::SharedBuffer m_storage;            // FROZEN; built as UniqueBuffer then moveToShared()
  Array<ImageSubresource> m_subresources; // mip_count entries, index m; MVP has no faces/array layers
};
// Invariants (checked at decode return; PPR_ASSERT + invalid_argument on violation):
// - m_dimension is ALWAYS image2d in MVP; 1D/3D/arrays/cubemaps are REJECTED with
//   function_not_supported (or fully represented if scope widens — never silently treated as 2D).
// - m_is_block == (m_tag != BlockTag::none); m_is_srgb == isSrgb(m_format).
// - Unblocked: m_block_w == m_block_h == 1, m_bytes_per_block == 4, m_subresources.size == m_mip_count.
// - Blocked: block extents/bytes match m_tag; row_pitch == ceil(w/bw)*bytesPerBlock (no redundant
//   stored copy may disagree — derive, don't duplicate).
[[nodiscard]] Expected<ImageAsset> decodeToRgba8(SharedBufferView bytes,
  std::string_view ext, ImageDecodeDesc, ImageUsage);
[[nodiscard]] Expected<ImageAsset> decodeToBlocks(SharedBufferView bytes,
  std::string_view ext, BlockTag want, ImageDecodeDesc);
// decodeToBlocks is passthrough/transcode of SUPPORTED compressed sources only
// (KTX2/DDS blocks the Mango decoder exposes). It NEVER recompresses PNG/JPG into blocks.
[[nodiscard]] inline hash_t contentHash(SharedBufferView v) noexcept
  { return hash::contiguousRange(v); }   // cross-load dedup key; NEVER hashValue(SharedBuffer) (owner identity)
```

Rules: reentrant + `const`-thread-safe CPU conversion (one mango `ImageDecoder` AND one
`UniqueBuffer` per job: `launch` throws on concurrent use; `g_imageServer` has no mutex).
Lifecycle: file→decode owned by the decode stage via `SharedBuffer::mapFile` (zero-copy
read-only; bridge to Mango as `ConstMemory{view.data(), size}`) → decode/process in a per-job
`UniqueBuffer` (`allocate`/`clone` → `materialize()` → checked `getMutableData()`; `scratch()`
workspace only, never stored) → freeze with `moveToShared()` → upload borrows `view.data()`
while the asset (or the cache's strong `SharedBuffer`) is held through `createTexture` return.
Before every `moveToShared()`: propagate `UniqueBuffer::materialize()` error, then verify
`isMaterialized()` AND the `getMutableData()`/`getBufferData()` result is valid; `SharedBuffer`
has NO `materialize()` — only the `isMaterialized()` observer. Frozen `SharedBuffer`s share
by value across threads; KTX2 keeps its per-decoder mutex + immediate `clone()` (single-slot
transcode cache). sRGB: `m_is_srgb = !header.linear` (KTX2 DFD forwards `transfer`); Auto fallback
color→sRGB, data→linear; never from filename. Y-flip: negative-stride `Surface` view is CPU-blit-only;
uploads always emit positive-pitch rows in sampler order.
`image::errc` map (concrete): unsupported extension → `invalid_argument`; decode failure →
`invalid_argument`; unsupported block target → `function_not_supported` (caller falls back to
`decodeToRgba8`); non-2D content → `function_not_supported`; missing/unmappable file is reported
at the `SharedBuffer::mapFile` boundary as `no_such_file_or_directory` (which returns
`Expected<SharedBuffer>`), NOT inside the byte decoders.

### 2.3 `engine.mesh:types` — CPU only; verbatim glTF copy (fork pre-converts RH→LH)

Positions/normals `(x,y,-z)`, tangents `(x,y,-z,w)`, file-order indices (already CW), UVs verbatim
V-down, nodes `S*M*S`, TRS `S*R*T` (row-vector). Matrices bitwise-copied (same row-major type).
Mango model assumed: metallic-roughness PBR core + clearcoat/sheen/anisotropy KHR-style modules,
of which MVP carries ONLY the core below.

```cpp
// StaticMeshVertex is a plain-float-array DTO (P1 interim vertex verdict):
// mango Vector members carry user-provided copy/dtor and can never be
// trivially copyable, which §2.4 upload + §3 bitwise-copy require. No math
// lives in the vertex type; conversion happens at the §3 boundary only.
struct StaticMeshVertex { float m_position[3]; float m_normal[3]; float m_texcoord[2];
  float m_tangent[4]; float m_color[4]; };
static_assert(std::is_trivially_copyable_v<StaticMeshVertex>);
static_assert(std::is_standard_layout_v<StaticMeshVertex>);
static_assert(sizeof(StaticMeshVertex) == 64);
static_assert(alignof(StaticMeshVertex) == 4);
static_assert(offsetof(StaticMeshVertex, m_position) == 0);
static_assert(offsetof(StaticMeshVertex, m_texcoord) == 24);
enum class EMeshAttribute : u32 { none = 0, position = 0x1, normal = 0x2, texcoord = 0x4,
  tangent = 0x8, color = 0x10 };           // PPR-owned typed flags; NO joints/weights bits in MVP
struct MeshPrimitiveRange { u32 m_start, m_count; i32 m_base; MaterialAssetId m_material; };
static_assert(std::is_trivially_copyable_v<MeshPrimitiveRange>);
// HONOUR stored base: append-path base=0 + remapped indices vs direct-glTF base=vert_count + file order.
struct StaticMeshAsset { Array<StaticMeshVertex> m_verts; Array<u32> m_indices;
  Array<MeshPrimitiveRange> m_prims; Box m_bounds; EMeshAttribute m_flags; };
struct UvTransformAsset { float2 m_scale{1.0f, 1.0f}; float2 m_offset{0.0f, 0.0f}; float m_rotation = 0.0f; };
struct MaterialImageSlot { ImageAssetId m_image = kInvalidImage; UvSetId m_texcoord{};
  UvTransformAsset m_transform;   // identity unless KHR_texture_transform authored; pack asserts for v1
  [[nodiscard]] bool enabled() const noexcept { return m_image != kInvalidImage; } };
enum class AlphaMode : u8 { opaque, mask, blend };
struct MaterialAsset { float4 m_base_color{1.0f, 1.0f, 1.0f, 1.0f}; float m_metallic = 1.0f;
  float m_roughness = 1.0f; float3 m_emissive{0.0f, 0.0f, 0.0f};
  float m_alpha_cutoff = 0.5f;             // Mask threshold only (MVP default: opaque ignores it)
  float m_occlusion_strength = 1.0f;       // from ImageSample.scale (only occlusion scalar Mango populates)
  float m_normal_scale = 1.0f;             // from ImageSample.scale (only normal scalar Mango populates)
  AlphaMode m_alpha_mode = AlphaMode::opaque;  // Opaque/Mask → pipeline state; blend REJECTED in MVP (§7)
  bool m_twosided = false;                 // → CullMode::None vs Back in pass pipeline key
  MaterialImageSlot m_base_color_map;      // rgba/sRGB
  MaterialImageSlot m_metallic_map;        // .b/Linear — SEPARATE slot from roughness (glTF sharing is the special case)
  MaterialImageSlot m_roughness_map;       // .g/Linear — different semantic channel; pack composites both
  MaterialImageSlot m_normal_map;          // .rgb/Linear
  MaterialImageSlot m_occlusion_map;       // .r/Linear
  MaterialImageSlot m_emissive_map;        // .rgb/sRGB
};
// MR resolved at convert: roughness=g(), metallic=b(), Linear; base_color rgba/sRGB, emissive rgb1/sRGB,
// normal rgb1/Linear, occlusion r/Linear (import_gltf.cpp:372-418).
// DEFERRED (nothing in mesh_bindless.slang v1 consumes them): clearcoat/sheen/anisotropy fields.
// NOT CARRIED: per-slot swizzle (implied by semantic — bake at process time), per-slot color_space
// (derived from semantic: base_color/emissive→sRGB, rest→linear).
// MVP scope is STATIC glTF/GLB ONLY. Skins/animations: rejected/deferred (no playback AND no storage).
// OBJ/FBX: deferred entirely (no factors/maps heuristics in MVP). Unsupported glTF material extensions
// (specular/transmission/volume/iridescence, KHR emissive_strength): converter returns
// function_not_supported when authored-and-required, else drops with a logged warning —
// the choice is explicit per extension, never silent widening.
struct ImageRef { std::string m_name, m_ext, m_rel_path; mem::SharedBuffer m_bytes; bool m_is_file = true; };
// Embed rule: clone Mango bytes (UniqueBuffer::clone → moveToShared) WHILE the Mango Scene lives;
// file refs use SharedBuffer::mapFile. After that nothing of Mango outlives the call — no
// shared_ptr<SceneResources> capture in the engine.
struct SceneNodeAsset { NodeId m_parent = kInvalidNode; float4x4 m_local; float4x4 m_world; };
struct SceneInstance { MeshAssetId m_mesh; NodeId m_node; MaterialAssetId m_materialOverride = kInvalidMaterial; };
// SceneAsset owns meshes + materials + image refs + nodes; instances reference them by typed ID.
struct SceneAsset { Array<StaticMeshAsset> m_meshes; Array<MaterialAsset> m_mats;
  Array<ImageRef> m_images; Array<SceneNodeAsset> m_nodes; Array<SceneInstance> m_instances; };
[[nodiscard]] Expected<SceneAsset> importAndConvert(
  const std::filesystem::path& dir, std::string_view file);
```

`mesh::errc`: `MANGO_EXCEPTION` → mapped code; silent-empty Scene → `invalid_argument`; missing
POSITION / bad indices → `invalid_argument`; joint data present → `function_not_supported`
(skins deferred); animation channels present → ignored for static bind pose ONLY when the static
snapshot is well-defined, else `function_not_supported`. `tangent.w` caveat (P0 probe):
mirroring should negate `w`, Mango keeps it — probe file-tangent vs MikkTSpace lighting, apply
`w=-w` if inverted. MikkTSpace failure is warning-only → fall back to computed tangents, never
silent flat normals. `mesh→image` type dependency REJECTED: `ImageRef` duplicated locally (no
convenient cross-dependency); the app-side pack step joins `ImageRef` → `decodeTo_*`.

### 2.4 Pass-owned GPU caches (members of `TrianglePass`/future `MeshPass`)

Teardown contract (authoritative; mirrors `App.Renderer.cpp:102-124` and
`App.Application.Editor.cpp:157-222`):

1. Application/Editor stops new submissions first (detach input callbacks, disable per-frame
   submissions, prevent new cache uploads).
2. Pass caches shut down BEFORE `Renderer::shutdown()`: each cache exposes
   `initialize()/shutdown() -> error_code` with retain-first-error + per-acquisition rollback.
   Shutdown order within the pass: caches → pipeline/program/layout/buffers (extend
   `TrianglePass::shutdown`, currently `App.Renderer.TrianglePass.cpp:87-98` which only
   releases pipeline/program/layout/buffer and must grow the cache stage).
3. `Renderer::shutdown()` runs `waitOnHost()` on the graphics queue, then unconfigures surfaces.
   Pass-owned GPU `ComPtr`s are therefore released while the device is idle and before surface
   teardown — never after.
4. Every stage preserves the FIRST error (`PPR_RETAIN_ERROR_ON_FAIL`) and continues best-effort
   cleanup; shutdown never hides a cleanup failure behind later work.
5. `ApplicationEditor::shutdown` integration: detector/input detach → pass shutdown
   (caches → pipeline) → UI service shutdown → input/viewport/window teardown →
   `Application::shutdown()` (renderer `waitOnHost` + surface unconfigure, RHI/shader, platform).
   The current file order (UI before triangle) is updated to this contract; impacted files §9.

Teardown proof (P2 Gate 3 C1, verified on disk 2026-09-22):
(a) New submissions stop first — the run loop has exited (`Application::run`
defers `shutdown()`), so no new `update`/`render` submissions or cache uploads
can start; `TrianglePass::shutdown` (`App.Renderer.TrianglePass.cpp`)
additionally `clearInstances()` and flips `m_caches_ready=false`, so any late
`uploadMesh/uploadTexture/packMaterial/submitInstance` fails closed with
`not_connected`. (b) Input detach precedes pass teardown —
`ApplicationEditor::shutdown` (`App.Application.Editor.cpp:160-193`) resets the
device-disconnect handle, shuts down the input latch, erases + shuts down UI,
then `m_triangle_pass->shutdown()` + reset, then input-context shutdown +
`clearInputListeners`. (c) Caches die before `waitOnHost` — pass shutdown runs
at Editor:183 while `Renderer::shutdown()` (`App.Renderer.cpp`, `waitOnHost()`
first) runs later inside `Application::shutdown()` (Editor:220 ←
`App.Application.cpp:178`). (d) Inverse-of-init with retain-first-error — init
builds render state → program → bag → texture → sampler → material (reverse
rollback on partial failure); shutdown releases instances → material → texture
→ bag → sampler → pipelines/program/layout/buffer, every fallible stage via
`PPR_RETAIN_ERROR_ON_FAIL` preserving the first error.

```cpp
// Generation-bearing range identity. The HANDLE is the SparseVector key (fails closed on
// stale/double release: tryGet miss → invalid_argument, never silent success). The RANGE is
// plain draw metadata passed by value — it carries NO identity and must not be used to release.
struct TriangleBagHandleRecord { BagBucketId m_bucket; u32 m_slot; u32 m_generation; };
struct TriangleBagRange { u32 m_vb_offset; u32 m_ib_start; u32 m_count; i32 m_base; Box m_bounds; };
// m_vb_offset is u32: the shader push takes uint and BufferDesc sizes are narrowed with
// safe_narrowing at upload; a bag that does not fit u32 is rejected with invalid_argument.
class TriangleBagCache {  // ONE class, all vertex types; bucket = hash(stride, attribute_mask, index_type)
  public:
   [[nodiscard]] std::error_code initialize(rhi::IDevice& device);
   [[nodiscard]] std::error_code shutdown();   // releases buckets; call before renderer waitOnHost
   template<typename V> [[nodiscard]] Expected<TriangleBagHandle>
     upload(std::span<const V> verts, std::span<const u32> idx);
   [[nodiscard]] Expected<TriangleBagRange> resolve(TriangleBagHandle) const;  // tryGet; miss → invalid_argument
   [[nodiscard]] std::error_code release(TriangleBagHandle);                   // erase; stale/double → error
};
// Draw rule: same m_bucket only (debug assert). MVP has static buckets only by construction.
// Bag upload keeps TYPED-span signatures (verts stay typed Arrays) — buffers appear only for staging copies.
// Bucket storage: FlatMap<hash_t, BagBucketId> layout→bucket + SparseVector<Bucket>; release() erases the
// range record (GPU memory itself is bump-allocated per bucket; free-list deferred to streaming phase).
class BindlessTextureCache {  // dedup FlatMap<DedupKey, TextureHandle>, refcount, pin-while-held
  public:
   [[nodiscard]] std::error_code initialize(rhi::IDevice& device, u32 textureBudget);
   [[nodiscard]] std::error_code shutdown();
   [[nodiscard]] Expected<TextureHandle> upload(const ImageAsset&);  // NO SamplerDesc: pass owns one shared sampler
   [[nodiscard]] std::error_code release(TextureHandle);             // stale/double → invalid_argument
   [[nodiscard]] Expected<TextureBindlessIndex> residentIndex(TextureHandle) const;  // stable slot; miss → error
   [[nodiscard]] rhi::ITextureView* view(TextureHandle) const;       // null on miss (observer only)
  private:
   // DedupKey = {contentHash bytes, width/height/format/mips/dimension}; lookup is hash-bucket +
   // FULL byte equality on pinned bytes (hash alone NEVER decides). Entry OWNS:
   // {ITexture, ITextureView, rhi DescriptorHandle, TextureBindlessIndex, SharedBuffer m_pinned,
   //  u32 m_refcount}. Descriptor mechanics live behind residentIndex()/bind bind helpers —
   // either via new engine.rhi aliases for DescriptorHandle or app-private in the pass;
   // RHI scope change (§5) covers more than DeviceDesc either way.
};
struct GpuTextureRefs { TextureBindlessIndex m_albedo, m_metallic_roughness, m_normal, m_emissive; };
static_assert(std::is_trivially_copyable_v<GpuTextureRefs>);
static_assert(sizeof(GpuTextureRefs) == 16);   // 4 named GPU-protocol slots, no raw-uint quartet
struct GpuMaterialFlags { u32 m_bits; };       // explicit mask, NO C++ bitfields on a GPU protocol type
inline constexpr u32 kGpuMaterialAlphaModeMask = 0x3u;      // bits [1:0]
inline constexpr u32 kGpuMaterialDoubleSidedBit = 0x4u;     // bit 2
static_assert(sizeof(GpuMaterialFlags) == 4);  // Slang mirror unpacks with the same masks
struct GpuMaterial {                         // Slang mirror: float4 rows + uint rows, row-major
  float4 m_base_color;                      // 16: albedo × alpha (opacity pre-baked)
  float4 m_emissive_metallic;               // 16: emissive.rgb + metallic
  float4 m_rough_alpha_occl_nscale;         // 16: roughness, alpha_cutoff, occlusion_strength, normal_scale
  GpuTextureRefs m_textures;                // 16: bindless Texture2D[] slots; kNoTexture = none → shader fallback
  UvSetId m_texcoord;                       // 4: shared set (pack VERIFIEs all enabled slots agree); serialized as u32
  GpuMaterialFlags m_flags;                 // 4
  u32 m_pad[2];                             // 8
}; // static_assert(sizeof == 80); 5 rows × 16 B keeps StructuredBuffer stride clean
static_assert(std::is_trivially_copyable_v<GpuMaterial>);
static_assert(sizeof(GpuMaterial) == 80);
static_assert(alignof(GpuMaterial) == 16);
static_assert(offsetof(GpuMaterial, m_textures) == 48);
// Occlusion shares the composited m_mr texture (ORM: R=occlusion-or-1, G=rough, B=metal) — no 5th slot.
// Shader fallbacks when slot = kNoTexture: albedo→m_base_color, mr→factors, normal→geometric normal,
// emissive→m_emissive factor over black. CPU handles stay SparseKeyId/seed-0-invalid; pack maps invalid→sentinel.
// Material packing mapping (pack() implements exactly this):
//   base_color+alpha → m_base_color; emissive.rgb+metallic → m_emissive_metallic;
//   roughness/alpha_cutoff/occlusion_strength/normal_scale → m_rough_alpha_occl_nscale;
//   resolved TextureHandles → residentIndex() → m_textures slots (missing → kNoTexture);
//   shared texcoord set → m_texcoord (u32); alpha_mode/twosided → m_flags bits.
class BindlessMaterialCache {  // pass-owned; stable slots into StructuredBuffer<GpuMaterial>
  public:
   [[nodiscard]] std::error_code initialize(rhi::IDevice& device, rhi::ISampler* sharedSampler);
   [[nodiscard]] std::error_code shutdown();
   [[nodiscard]] Expected<MaterialHandle> pack(const MaterialAsset& asset,
     std::span<const TextureHandle> resolvedTextures);  // caller resolves ImageAssetId→TextureHandle first
   [[nodiscard]] std::error_code release(MaterialHandle);  // stale/double → invalid_argument
   [[nodiscard]] rhi::IBuffer* materialBuffer() const noexcept;  // StructuredBuffer<GpuMaterial> for g_materials
};
// Shared sampler ownership: TrianglePass creates and owns ONE shared ISampler (≤8 sampler set,
// D3D12 heap cap 2048 noted in §4) and lends the raw pointer to BindlessMaterialCache::initialize
// (non-owning view). Pass shutdown destroys material cache first, then the sampler. Sampler state
// is NEVER part of the texture dedup key.
```

### 2.5 Buffer lifecycle + thread rules

1. file→decode owned by the decode stage: `SharedBuffer::mapFile` (zero-copy read-only); bridge to
   Mango as `ConstMemory{view.data(), size}`. `UniqueBuffer::mapFile` only for genuine R/W.
2. decode→process in a per-job `UniqueBuffer` (`allocate`/`clone` → `materialize()` → checked
   `getMutableData()`); `scratch()` workspace only, never stored.
3. process→frozen via `moveToShared()` (zero-copy owner move; internally materializes first —
   callers still propagate ITS error AND verify `isMaterialized()` + result validity before use).
   `makeOwned() &&` only for un-owned (mapped/subspan) uniques.
4. frozen→upload borrows `view.data()`; the asset (or cache's strong `SharedBuffer`) is held through
   `createTexture` return. `isMaterialized()` check precedes every upload (const access is
   silent-empty when unmaterialized — hence the explicit check).
5. upload→cache stores a STRONG `SharedBuffer` per entry (re-upload keep-alive). `WeakSharedBuffer`
   is observers-only (expired `pin()` is invalid — never a cache key).
6. Threads: frozen `SharedBuffer`s share by value; `UniqueBuffer` + mango decoder per job; KTX2 keeps
   per-decoder mutex + immediate `clone()`. No explicit COW policy (`moveToUnique` covers it).
   CPU conversion stages are reentrant/const-thread-safe; GPU pass caches
   (`TriangleBagCache`, `BindlessTextureCache`, `BindlessMaterialCache`) are
   RENDER-THREAD CONFINED (no internal mutex; concurrent upload is a caller bug).
7. Unknown-size outputs: Mango-known size → `allocate(size)`; Mango view → `clone(view)` (best-fit).

## 3. Conversion table (Mango fork → PPR, static glTF path)

| Mango | PPR | Rule |
|---|---|---|
| `float32x2/3/4`, `matrix4x4`, `Box` | `float2/3/4`, `float4x4`, `Box` | bitwise copy (same types, row-major, translation row 3) |
| mango `Vertex` floats | `StaticMeshVertex` `float[N]` arrays | component copy at the boundary only (verbatim LH `(x,y,-z)`; `w` verbatim pending probe) — mango vectors are not trivially copyable, so no member-wise assignment; the array struct itself stays trivially copyable + standard-layout and memcpy-safe |
| position / normal | `m_position` / `m_normal` | verbatim — fork already `(x,y,-z)` |
| tangent | `m_tangent` | verbatim **pending P0 probe** (`w` may need negate; MikkTSpace regen is correct fallback) |
| texcoord | `m_texcoord` | verbatim, NO V-flip (V-down matches) |
| indices | `m_indices` | file order kept (already CW-outside); honour per-prim `m_base` |
| `Material` + `ImageSample` | `MaterialAsset` | per §2.3 slot rules (MR split, sRGB map, scale fields) |
| `ImageSource` | `ImageRef` | file→`mapFile` ref; memory→clone while `files` alive; empty skipped |
| `Node.transform` | `SceneNodeAsset::m_world` | accumulate `parent*local` (row-vector `S*R*T`); keep BOTH local and world |

Embed-lifetime extension: the fetch-cache fork truncates GLB blob lifetime (local `GltfDataBuffer`, `Asset`-owned sources), so PPR re-resolves embeds from its own `mapFile` mapping — mango views are never read.

## 4. GPU upload rules (verified against vendored slang-rhi, no divergence)

- `IDevice::createTexture(desc, initData, out)`: `initData` = one `SubresourceData` per subresource,
  count = layers × mips; MVP is 2D-only so index is the mip `m` (proven D3D11/CUDA/CPU/shared paths
  + fixtures).
- `SubresourceData{data,row_pitch,slice_pitch}`: tight pitches accepted (backend pads internally via
  `min(src,dst)` row copy); `slice_pitch` MUST be set (2D: `row_pitch*height` semantics); block math
  `row_pitch = ceil(w/bw)*bytesPerBlock`. `getSubresourceLayout()` optional for upload, REQUIRED for
  the `readTexture` debug path (exact-match validation).
- `TextureDesc{type=TextureType::Texture2D,size,array_length=1,mip_count,format,usage=ShaderResource,
  default_state=ShaderResource,memory_type=DeviceLocal}`; static data uploads with initData
  (TrianglePass VB precedent), dynamic per-frame via Upload heap + `mapBuffer` (ImGui VB/IB precedent).
  Non-2D `TextureType` values are rejected by the cache in MVP.
- `DeviceDesc::bindless{buffer1024, texture4096, sampler128, combined4096}` — create-time only.
- Format table (each gated by `getFormatSupport(ShaderSample)`, RGBA8 fallback): RGBA8Unorm(Srgb),
  BC1/3/4/5/7(UnormSrgb), ASTC4x4/6x6/8x8(UnormSrgb). ETC/PVRTC/other-ASTC/Basis-mismatch → RGBA8.
  Blocks: color→BC7-sRGB (BC1-sRGB opaque / BC3-sRGB alpha fallback), normal→BC5-linear,
  single-channel→BC4. Mips: KTX2 embedded when present; CPU-generate only uncompressed RGBA
  (color bicubic, data box); never decode→recompress. Samplers: pass owns ONE shared sampler
  (§2.4); no per-texture sampler.
- Staging recipe (ImGui font-texture template, `App.UI.ImGui.cpp:544-599`): DeviceLocal texture with
  `CopyDestination` usage + `defaultState=CopyDestination` → Upload staging buffer with initData →
  `copyBufferToTexture` → `setTextureState(ShaderResource)` → submit + `waitOnHost()` → `getDefaultView()`.
  The returned view + stable `TextureBindlessIndex` + `rhi` descriptor handle are stored in the
  cache entry (§2.4); per-frame binding uses the existing `ShaderCursor::setBinding` idiom
  (`App.UI.ImGui.cpp:411-412` set sampler/texture bindings; bindless array slots follow the same
  cursor shape with descriptor handles resolved from the cache, NOT raw uint slots).

## 5. RHI + `engine.rhi` scope (beyond only device creation)

1. Device creation — in `SlangRhiService::initialize` (`lib/engine/rhi/RHI.cpp:256-271`), after
   `requiredFeatures` wiring: `desc.bindless.textureCount = 4096;`
   `desc.bindless.combinedTextureSamplerCount = 4096;` (leave `samplerCount = 128`,
   `bufferCount = 1024`). Do NOT add `Feature::Bindless` to requiredFeatures (would fail weak
   hardware); caches check `hasFeature(Bindless)` at init (test pattern `test-bindless.cpp:186-189`).
   Changing counts later = device recreation (no update API).
2. Bindless-descriptor vocabulary — `RHI.cppm` currently re-exports `ISampler`/`SamplerDesc` but NO
   descriptor-handle type. Either (a) add explicit `engine.rhi` aliases for the slang-rhi
   descriptor-handle/binding types the pass needs, or (b) keep ALL descriptor mechanics app-private
   inside `TrianglePass` caches and bind only through `ShaderCursor`. The choice is recorded at P2;
   what is NOT allowed is reaching past `engine.rhi` into slang-rhi directly from `engine.app`
   code — descriptor access goes through the chosen seam only.
   P2 RECORD (2026-09-22): choice **(a)**. `RHI.cppm` gains `DescriptorHandle` /
   `DescriptorHandleAccess` / `DescriptorHandleType` / `Feature` aliases plus the
   `kBindless*Budget` constants; pass caches use `rhi::` names only and bind
   per-handle via `ShaderCursor::setDescriptorHandle` (Gate 1 scalar-Handles
   fallback, no `Handle[N]`). No other slang-rhi surface crosses into `engine.app`.

## 6. Bindless shader (`assets/shaders/mesh_bindless.slang`, new file)

Coherent draw model (DECIDED — no longer deferred): PURE StructuredBuffer fetch. The written
shader below is the contract; the CPU encode matches it exactly.

```slang
struct BagVertex { float3 m_position; float3 m_normal; float2 m_uv; float4 m_tangent; float4 m_color; };
// CPU StaticMeshVertex stride is 64 B (plain-float arrays, 12+12+8+16+16); Slang mirror unchanged.
struct MeshPush { uint m_vb_offset; uint m_ib_start; uint m_index_count; uint m_material; float4x4 m_model; };
// m_material is the RESOLVED material slot (StructuredBuffer<g_materials> index from the
// MaterialHandle record), not the handle itself — handles are resolved at encode time.
StructuredBuffer<BagVertex> g_vertices : register(t0);
StructuredBuffer<uint>      g_indices  : register(t1);
StructuredBuffer<GpuMaterial> g_materials : register(t2);   // mirrors §2.4 (5×16 B rows)
uniform Texture2D<float>.Handle g_textures[4096];
uniform SamplerState.Handle g_sampler;
// VS (sv_vertex_id in [0, index_count)): idx = g_indices[push.m_ib_start + vid] + push.m_vb_offset;
//     BagVertex v = g_vertices[idx];
//     float4 world = mul(float4(v.m_position, 1.0f), push.m_model);
//     output = mul(world, m_view_projection); pass uv/normal/material.
// FS: GpuMaterial mat = g_materials[push.m_material];
//     float4 albedo = (mat.tex == kNoTexture) ? mat.m_base_color
//                  : g_textures[NonUniformResourceIndex(mat.tex)].Sample(g_sampler, uv) * mat.m_base_color;
```

C++↔Slang mirror table (hardening: check this, not oral tradition):

| C++ (CPU) | Size / offsets | Slang mirror | Shader stride |
|---|---|---|---|
| `StaticMeshVertex` (position @0, normal @12, uv @24, tangent @32, color @48) | 64 | `BagVertex` (float3/float3/float2/float4/float4) | 64 B `StructuredBuffer` stride |
| `GpuMaterial` (3×float4 rows + `uint4 m_textures` @48 + texcoord/flags/pad) | 80 | `GpuMaterial` (float4×3 + uint4 + uint + uint + uint2) | 80 B stride |
| `PushScalars` (vb_offset/ib_start/index_count/material/base_vertex) | 20 | `PushScalars` (uint×4 + int) vertex entry-param | 20 B entry-param block |
| `float4x4` model matrix | 64 | `float4x4 g_model` vertex entry-param | 64 B entry-param block |

Enforcement: `static_assert`s on `sizeof` (+ `m_textures` @48) in
`App.Renderer.GpuCaches.cppm`, `sizeof(PushScalars) == 20` in
`App.Renderer.TrianglePass.cpp`, a runtime stride-check in `encodeInstance_`
(CPU `sizeof` vs buffer `elementSize`, fail-closed), and `makeFullRange()`
for every `setBinding` (bare `Binding()` banned — the default range reads
empty across the module boundary).

Bindless hardening (adjudication SPLIT — P3 scalar stands): texture/sampler
binding is NORMATIVELY scalar — one `uniform Texture2D<float4>.Handle` per
material slot plus one shared sampler, all as entry-point params (root
constants); host binds per-name via `setDescriptorHandle` (+ white
fallback); the cursor array-element path is banned. Divergent indices use
Slang `nonuniform()`, never HLSL-native `NonUniformResourceIndex`. P0c
`Handle[4096]` probe verdict amended to compile-only (SPIR-V/DXIL-SM6.6
rc=0; zero slang-rhi runtime contract).

Container upgrade (only if eviction/dedup pressure demands it):
`StructuredBuffer<DescriptorHandle<Texture2D<float4>>>` heap + plain-uint
slots in `GpuMaterial` — raw `Handle[N]` root params never.

CPU encode per instance (mirrors `TrianglePass::render:58-71` + ImGui `:409-412`):

- `resolve(handle) → TriangleBagRange` (§2.4) for `{m_vb_offset, m_ib_start, m_index_count}`;
  `residentIndex(textureHandle)` / `materialBuffer()` for material/texture bindings.
- `bindPipeline → ShaderCursor` → `cursor["g_frame"].getDereferenced()` + `setData(FrameConstants)` →
  `cursor["g_bag"].setBinding(bagVB)` / `setBinding(bagIB)` → `cursor["g_mat"].setBinding(materialBuffer)` →
  per-resident-texture descriptor binding via the §5 seam → `cursor["g_push"].setData(&push)` →
  `setRenderState(viewport/scissor)` with NO vertex-buffer/index-buffer bindings →
  NON-INDEXED `draw({vertexCount = range.m_count, ...})` with `sv_vertex_id` driving the manual
  `g_indices` lookup. There is deliberately NO `drawIndexed` and NO index-buffer `RenderState`
  in this model — fixed-function index fetch is not used.

## 7. `TrianglePass` refactor + `ApplicationEditor` ownership flow (method-by-method diff)

Ownership: `ApplicationEditor` owns the loaded `SceneAsset`, the decoded `ImageAsset`s, and the
`TrianglePass` (which owns its three GPU caches + shared sampler). Client/editor code owns scene
state and SUBMITS work; the pass encodes submitted work and owns only GPU-resident caches.

Loading/upload/submission flow (happy path; every fallible step returns `error_code`/`Expected`
with partial rollback — cache entries acquired so far are released in reverse order):

1. `importAndConvert(dir, file)` → `SceneAsset` (editor-owned).
2. For each `ImageRef`: `SharedBuffer::mapFile` (missing file → `no_such_file_or_directory` HERE)
   or clone embedded bytes → `decodeToRgba8`/`decodeToBlocks` → `ImageAsset` (editor-owned).
3. `trianglePass.uploadScene(asset, images)`: bag `upload()` per mesh → texture `upload()` per image
   (dedup hit returns existing handle + bumps refcount) → `pack()` per material. Narrow pass APIs:
   `uploadMesh(std::span<const StaticMeshVertex>, std::span<const u32>)`,
   `uploadTexture(const ImageAsset&)`, `packMaterial(const MaterialAsset&, resolvedTextures)`,
   `submitInstance(TriangleBagHandle, MaterialHandle, const float4x4&)`, `clearInstances()`.
4. Per frame: editor builds `SceneView` (CameraSnapshot + RenderView) and the instance list
   `{TriangleBagHandle, MaterialHandle, model = node world}`; pass `update()` snapshots it;
   `render()` encodes per §6.
5. Removal/teardown: `releaseInstance` → `release(MaterialHandle)`/`release(TextureHandle)`/
   `release(TriangleBagHandle)` (refcounted; stale/double → `invalid_argument`, fail closed) →
   full pass `shutdown()` per §2.4.

- Members: DELETE `m_vertex_buffer` (single triangle); ADD pass-owned `TriangleBagCache`,
  `BindlessTextureCache`, `BindlessMaterialCache`, one shared `ISampler`, per-instance list
  (+ keep `m_camera_view`, pipeline/program/layout/key).
- `initialize`: build the bindless program/layout for StructuredBuffer fetch (NO fixed-function
  `InputElementDesc` for geometry — §6 decided) + `caches.initialize(device)` with rollback on
  partial failure (release each acquired cache in reverse order, retain first error).
- `createShaderProgram_`: load `mesh_bindless.slang` (same `loadModuleFromFile` + entry-point pattern).
- `createRenderPipeline_`: MVP supports OPAQUE + MASK only. Pipeline cache keyed by
  (target signature + `m_twosided` cull state + `m_alpha_mode` alpha state). BLEND is explicitly
  deferred: any `AlphaMode::blend` material is REJECTED with `function_not_supported` until a
  sorting + depth-write policy is fully specified. Never claim "one pipeline" while variants exist —
  the key above IS the variant policy.
- `update`: unchanged (snapshot copy) + per-instance list from client
  (`{bagHandle, materialHandle, model}`; handles resolved to ranges/indices at encode time).
- `render`: per-instance encode (§6) across the pipeline-keyed variants; `uploadFrameConstants_` unchanged.
- `shutdown`: caches → sampler → pipeline/program/layout (retain-first-error), before renderer `waitOnHost`
  per §2.4. Thread model: CPU conversion (§2.2–2.3) is thread-safe; these pass caches are
  render-thread confined.

## 8. Staging, fixtures, tests

- Staging (`build-system` skill): extend `game/CMakeLists.txt` + `lib/engine/tests/asset/CMakeLists.txt`
  POST_BUILD with `copy_directory assets/textures → <exe>/textures` and `assets/meshes → <exe>/meshes`
  (today only shaders are staged). Runtime resolves via `getContentDir()/"meshes"/"textures"`.
- Fixtures (add, <100 KB each, CC0): `textured_quad.glb` (1 quad, POSITION+NORMAL+TEXCOORD, baseColor PNG
  embedded — covers GLB-embed path) + `textured_box.gltf + .bin + .png` (external-URI path).
  Provenance + license recorded in `assets/textures/PROVENANCE.md` + `assets/meshes/PROVENANCE.md`
  (source, author, CC0 statement). Fixtures stage only on `engine.tests.asset` (same POST_BUILD on the
  asset test target); core tests never depend on staged directories.
- Test homes/patterns: add `engine.tests.asset` in `lib/engine/tests/asset/`, module
  `engine.tests.asset`, suite root `pP::tests::asset`, and CTest `EngineAssetUnitTests`. It uses the
  shared `engine.tests` `parseCli`/`runSuite` runner and follows the core/app
  `main.cpp` / `*.Tests.cppm` / `*.Tests.cpp` + `Tests()`-accessor + extern-umbrella
  (MSVC C1001 workaround) pattern. It owns `engine.image` decode/format/block/hash/error tests,
  `engine.mesh` import/convert/material/rejection tests, asset ID/layout asserts, asset GPU cache +
  bindless tests, the final textured-render integration test, and texture/mesh/shader fixture staging.
  The final gate reuses the `App.PixelReadback.Tests.cpp` TestApp boot + `renderToTexture` +
  `waitOnHost` pattern with MULTI-pixel DISTINCTIVE texels — e.g. corners/center with different
  colors — so a base-color fallback CANNOT pass. Its test target links
  `engine.image + engine.mesh + engine.app + engine.rhi + engine.shader + engine.core + engine.math +
  engine.tests`, with private GLFW for GPU integration; test dependencies do not change the production
  graph. `engine.tests.core` retains only the P0 `SparseKeyId` regression because it is an
  `engine.core` contract. Existing non-asset app tests remain in `engine.tests.app`.

## 9. Module graph, impact list, phases, validation, risks, gates

Authoritative graph delta (AGENTS.md §Repository facts): ADD `engine.image`, `engine.mesh` rows:

```text
engine.app -> engine.core, engine.math, engine.shader, engine.rhi, engine.image, engine.mesh
engine.image -> engine.core, engine.math (+ PRIVATE mango-image)
engine.mesh  -> engine.core, engine.math (+ PRIVATE mango-import3d)
```

AGENTS.md + root `codemap.md` are updated with the new rows. `lib/engine/CMakeLists.txt` gains
`image`/`mesh` subdirs. New `lib/engine/image/CMakeLists.txt` (+ `codemap.md`),
`lib/engine/mesh/CMakeLists.txt` (+ `codemap.md`) register umbrella + partitions and link
PRIVATE mango deps. Add `lib/engine/tests/asset/CMakeLists.txt` and suite files, then wire the target
into `lib/engine/tests/CMakeLists.txt` and the `run-engine-tests` aggregate. `engine.tests.asset`
receives the §8 test dependencies; no production dependency edge changes.

Exact changed-file inventory (one change = source file + its CMake registration):

- ADD: `lib/engine/image/*.{cppm,cpp}` (`:types`, `:decode`), `lib/engine/mesh/*.{cppm,cpp}`
  (`:types`, `:convert`), `assets/shaders/mesh_bindless.slang`, `assets/textures/*`,
  `assets/meshes/*` + fixtures, `PROVENANCE.md` × 2, core fix + P0 regression tests (§2.0),
  `lib/engine/tests/asset/CMakeLists.txt` + asset suite `main.cpp` / `*.Tests.cppm` / `*.Tests.cpp`
  files (image/mesh CPU, ID/layout, GPU cache/bindless, final gate).
- MODIFY: `AGENTS.md` (graph), root + `lib/engine/` + `lib/engine/image` + `lib/engine/mesh` +
  `lib/engine/tests/CMakeLists.txt`, `lib/engine/tests/{core,app,asset}` CMake, `TrianglePass.{cppm,cpp}`
  (caches, sampler, narrow APIs,
  pipeline-key variants, shutdown order), `RHI.{cppm,cpp}` (§5 device + descriptor seam),
  `ApplicationEditor` (`App.Application.Editor.{cppm,cpp}` ownership/loading/submission/teardown),
  game + asset-test staging (`game/CMakeLists.txt`, `lib/engine/tests/asset/CMakeLists.txt`),
  asset-suite registration + `run-engine-tests` aggregate, `lib/engine/{image,mesh}/codemap.md`,
  `lib/engine/tests/codemap.md`, root `codemap.md`.

Phases (each phase ends with its gate; re-review only on changed decisions/risks):

- **P0 probes + skeletons + core fix** (go/no-go): §2.0 `SparseKeyId` fix + regression tests FIRST →
  module skeletons (umbrella + empty partitions, CMake wired, `FILE_SET CXX_MODULES`) → bindless-array
  probe → tangent-w probe → 8-way hammer → `Numeric` capability + layout checks →
  `materialize()`/validity checks (§2.5 rule 3). Exit: core fix merged, fallback decisions recorded,
  skeletons build clean.
- **P1 CPU pipeline** (2 parallel lanes; lane A owns shared CMake/test files): image decode
  (P1a RGBA → P1b blocks) + mesh convert + `engine.tests.asset` fixtures/staging. **Gate 2:** API
  surface, error policy, thread-safety, no-`rhi`-leak, MVP scope rejections (§2.2–2.3) proven by
  asset-suite tests.
- **P2 GPU caches + bindless budget** (1 lane + parallel structure scan): three caches, §5 RHI edit +
  descriptor seam, shared sampler, pipeline-key variants (§7), and `engine.tests.asset` GPU/cache +
  bindless coverage. **Gate 3:** pass-owned, teardown-inverse (§2.4), handle semantics incl.
  stale/double fail-closed, heap budget.
- **P3 shader + gate** (1 lane): §6 shader, pass refactor, `ApplicationEditor` flow (§7), and the
  `engine.tests.asset` final-gate test with distinctive-texel multi-pixel asserts (§8). **Gate 4:** GPU
  correctness.
- **Phase R** bounded remediation only.
- Phase validation (proportionate; full matrix only at the end): per phase,
  `cmake --preset msvc-dev` → build → `ctest -R EngineAssetUnitTests` for asset work (and touched
  suites, shuffled where enabled) → changed-file inspections → `code-reviewer` gate. FINAL validation:
  configure + build `msvc-dev` / `msvc-live` / `msvc-rel`, shuffled core + app + asset suites where
  enabled, changed-file inspections, `code-reviewer` gate. New-file + CMake-registration = one change;
  no whitespace-only hunks.
- Risks: handle-arrays→scalar fallback · `w`→negate+MikkTSpace net · weak GPU→gates+RGBA8 ·
  KTX2 race→mutex+clone · silent-empty→`invalid_argument` · scope creep→deferred (§2.3 rejections).
  The old `materialize()` no-op / `Numeric`-fallback / `.slim`-pointer risks are retired by
  §2.0/§2.5 and this section respectively.
