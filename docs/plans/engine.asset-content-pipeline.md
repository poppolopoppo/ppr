# engine.asset Content Pipeline — Consolidated Spec

> Status: DRAFT — not approved, no implementation performed. This document is
> the execution-ready spec for the future `engine.asset` module; it consolidates
> the content-pipeline diagrams in `docs/diagrams/engine.asset/`. Where this
> spec conflicts with the older diagrams, this spec wins.

## 1. Goal

XNA-style content pipeline (asset = path + importer + processor) with a
vend-only, caller-owned HANDOFF `IContentService` in a new top-level
`engine.asset` module:

- Per-type move-out loads (`loadImage`, `loadStaticMesh`, `loadShaderModule`);
  ownership stays client's; returned CPU bytes are independently owned.
- Single project-level JSON manifest is the authoring truth; pack TOC/catalog
  is generated truth and ships.
- No runtime eviction in shipping; per-scope ownership (level scope size =
  Σ assets, cooker-reportable).
- Acceptance: `msvc-rel` `app.game` boots with NO loose assets staged; a
  missing or corrupt pack fails closed.

## 2. Module graph, files, and lifecycle

```text
engine.asset -> engine.core, engine.math, engine.image, engine.mesh, engine.shader
engine.app   -> engine.asset
```

`engine.asset` never imports `engine.app` or `engine.rhi`. Runtime target
`engine.asset` (one change = source file + its CMake registration):

- `Asset.cppm` — umbrella, re-exports only.
- `Asset.Content.cppm` / `.cpp` — `IContentService : IService`
  (`initialize(const ContentDesc&)`, `shutdown()`), `ContentDesc`, `ContentErrc`,
  `AssetContentService`, `createContentService()`.
- `Asset.Manifest.cppm` / `.cpp` — manifest data model + parse/validate.
- `Asset.Pack.cppm` / `.cpp` — pack reader DTOs, checked `construct()`, `cookPack()`
  declarations (definitions in the host-only cooker target).
- `CMakeLists.txt` — `setup_ppr_project()`; simdjson wiring (see §4);
  `EXTERNAL_SYSTEM_PRIVATE_DEPS` for simdjson.
- `codemap.md` — directory contract.

Producer-owned codecs (not in `engine.asset`): `engine.image:serialize` and
`engine.mesh:serialize` partitions own the DTO↔runtime conversions and the
checked offset/size/alignment walks for their payloads. `engine.asset` owns
only the pack container.

`lib/engine/CMakeLists.txt` inserts `add_subdirectory(asset)` after `shader`
and before `app`. `AGENTS.md` graph row and `codemap.md:43` both need the
image/mesh rows when the module lands (the `AGENTS.md:20-26` row currently
omits image/mesh).

HOST-ONLY cooker target (`ppr_asset_cooker`, not linked into the runtime or
shipping target): importer/processor dispatch, `cookPack()` definitions,
manifest validation CLI, staging. Shipping links only `engine.asset` plus its
declared private deps — never Mango importer/compiler code paths.

`createContentService()` returns an owning result (`Expected<ServiceOwner>` or
equivalent RAII wrapper, not a raw pointer). `Application` stores the service
as an owning member declared so it outlives `m_services` and is destroyed
before it; `Application::initialize()` calls
`service.initialize(desc)` then `m_services.insert_or_assign(...)`.

Headless: a headless `ApplicationDomain` (no rendering/UI) may construct and
initialize the service for CPU-side loads; no `TrianglePass`/GPU work occurs.
Headless shutdown is the same order minus the pass/RGPU stages.

No `service/App.Service.Content.cppm` in `engine.app`; no `IUploadSink` (see §3).

## 3. Vend-only handoff service (caller-owned, no handles)

```cpp
struct ServiceOwner { /* owns the service, shutdown on destruction */ };

[[nodiscard]] Expected<ServiceOwner> createContentService();

struct IContentService : IService {
  [[nodiscard]] error_code initialize(const ContentDesc&);
  [[nodiscard]] error_code shutdown();
  [[nodiscard]] error_code mountPack(string_view name); // v1: no id, single namespace
  [[nodiscard]] error_code unmountAll();
  [[nodiscard]] error_code loadImage(string_view name, ImageAsset* out);
  [[nodiscard]] error_code loadStaticMesh(string_view name, mesh::SceneAsset* out);
  [[nodiscard]] error_code loadShaderModule(string_view name, SharedModule* out);
};
```

`shutdown()`, `mountPack()`, and `unmountAll()` return `error_code` because a
missing pack, a lifecycle violation, and a first cleanup failure are
operationally observable outcomes. `shutdown()` retains the FIRST cleanup
error while attempting every release. No `noexcept` is claimed: v1 loads and
the service destructor are not `noexcept` today.

Output behavior, per type:

- `loadImage` / `loadStaticMesh`: on any error the out-param is untouched; on
  success the owned value is move-assigned, REPLACING any existing contents
  (the caller must not hold borrows into the previous contents). A null
  out-param is `invalid_argument`.
- `loadShaderModule`: on any error the out-param is untouched; on success the
  raw session-owned BORROWED view is assigned. Assignment copies a borrowed
  pointer; it transfers no ownership. The view is invalid after shader
  shutdown, and the caller must not hold it across service shutdown.

`mountPack(name)` takes no id in v1 (typed-IDs-only; the id was unused by
loads and paired only with `unmountAll`). A missing or invalid pack is
reported through its `error_code`. A second concurrent pack requires a typed
`PackIdentity` plus duplicate-name rejection and defined mount priority —
out of v1 scope.

`unmountAll()`: unloads the pack session and drops the service's MappedFile
reference, returning the first release error. It does NOT invalidate CPU
bytes the client already owns (v1 loads copy payload core ranges into
independently owned `SharedBuffer`s; image storage is already a strong
`SharedBuffer`). Idempotent after a successful mount; a missing mount is not
an error. After `shutdown()`, every load returns `not_initialized` WITHOUT
touching the shader service.

External serialization (normative): ALL `IContentService` methods
(`initialize`, `mountPack`, `load*`, `unmountAll`, `shutdown`) are externally
serialized by the caller; the service has NO internal mutex in v1. One
serialization domain covers manifest state, pack mapping, the lifecycle flag,
and the shader service together.

Distinct-output CPU loads MAY be moved across threads by the caller: image
decode is reentrant per job (per-job Mango decoder + `UniqueBuffer`, mip
generation mutates only the supplied asset); `Mesh.Convert.cpp:1375-1388`
shows `printEnable` is one `std::call_once` and `importScene` state is per
call. Concurrent calls targeting the SAME out object are caller misuse (data
race). Shader loads stay on the serialized service-owner thread (§7).

No `reload()` API: dev reload = load again + re-upload.

## 4. Project-level JSON manifest (authoring truth)

One diff-friendly JSON file at the content root (`assets/manifest.json` in
the repository). `content_root` is relative to the file's own directory.
Runtime relation: `getContentDir()` returns the executable's parent
(`App.Application.cpp:145-147`); staging currently copies
`assets/{shaders,textures,meshes}` beside the executable
(`game/CMakeLists.txt:22-46`). The manifest's `content_root` is the parent of
`meshes/`, `textures/`, and `shaders/` entries, so a dev content dir holds
`manifest.json` beside those three staged subdirectories. Shipping does not
stage the manifest; the service locates the manifest from the pack build,
while dev locates it under `getContentDir()`.

```json
{
  "version": 1,
  "content_root": "assets",
  "packs": [
    { "name": "content.ppak", "tags": ["base"] }
  ],
  "assets": [
    { "name": "meshes/textured_box", "source": "meshes/textured_box.gltf",
      "type": "static_mesh", "tags": ["base"],
      "deps": ["textures/box_albedo"] },
    { "name": "textures/box_albedo", "source": "textures/box_albedo.png",
      "type": "image", "tags": ["base"],
      "options": { "usage": "color", "mips": "incremental" } },
    { "name": "shaders/mesh_bindless", "source": "shaders/mesh_bindless.slang",
      "type": "shader", "tags": ["base"],
      "options": { "targets": ["dxil", "spirv"] } }
  ]
}
```

JSON parsed with simdjson. simdjson is a DIRECT dependency (`vcpkg.json:12`;
Mango already uses it). CMake needs NEW wiring — no
`cmake/external/*simdjson*` exists; add `find_package(simdjson CONFIG REQUIRED)`
plus the private link in `engine.asset` CMake. API confined to the `.cpp`;
plain structs cross the `.cppm` boundary.

`Asset.Manifest.cppm` (plain data + parse/validate decls):
`enum class AssetType : u8 { image, static_mesh, shader }`; typed option
structs `ImageOptions{usage, mips}`, `MeshOptions{}` (none in v1),
`ShaderOptions{targets}`; `ManifestEntry{m_name, m_source (relative to
content_root), m_type (defaulted from extension, overridable), m_tags,
m_deps, m_options}`; `ManifestPack{m_name, m_tags}`;
`ContentManifest{m_version, m_entries, m_packs}`. `name` is both pack TOC key
and `load*` argument. A variant over the three typed option bags replaces an
untyped options bag so unknown-key rejection is meaningful.

Validation (fail-closed): version equality (newer-than-reader rejected);
unknown top-level or per-type option keys are errors; duplicate `name` is an
error; missing `source` file or dangling `deps[]` is an error at cook/init;
every pack must match ≥1 entry. Parsed once (dev service init, cooker startup)
into an immutable name→record map using simdjson OnDemand, single forward
pass, no DOM retention.

Built-in dispatch only: type → importer/processor is a fixed table keyed by
`AssetType` (extension-defaulted, overridable by the `type` field). No
generic importer/processor extensibility in v1.

## 5. `.ppak` v1 wire ABI

All integers little-endian, fixed width, explicit field order. Hash
algorithm for every domain: FNV-1a 64-bit (`u64`), full 8 bytes stored.

Footer (at EOF), 32 bytes:

| field | type |
|---|---|
| `magic` | `u32` = `'PPAK'` |
| `version` | `u32` = 1 |
| `tocOffset` | `u32` (from pack start) |
| `tocSize` | `u32` (bytes) |
| `entryCount` | `u32` |
| `footerHash` | `u64` (FNV-1a over the first 24 footer bytes) |

TOC entry, 72 bytes + dependency vector:

| field | type |
|---|---|
| `nameHash` | `u64` |
| `kind` | `u32` (payload kind + per-kind payload version packed) |
| `coreOffset`, `coreSize` | `u32`, `u32` |
| `coreHash` | `u64` |
| `metadataOffset`, `metadataSize` | `u32`, `u32` (sentinel `0xFFFFFFFF` when absent) |
| `targetTableOffset`, `targetTableSize` | `u32`, `u32` (sentinel when absent) |
| `depCount` | `u32` |
| `targetMask` | `u32` (legacy filter hint; not the shader representation) |

Shader target subtable entry, 40 bytes: `target` (`u32`), `codeOffset`,
`codeSize` (`u32`, `u32`), `codeHash` (`u64`), `reflectionOffset`,
`reflectionSize` (`u32`, `u32`), `reflectionHash` (`u64`), `compilerVersion`
(`u32`). Per-blob hashes are FNV-1a 64.

Dependency encoding: `depCount` immediately followed by `depCount` × `u64`
nameHash values, stored inside `tocSize`; duplicate dependency hashes are a
validation error.

Limits: every offset/size is `u32`; a negative direction uses `i32` capped at
2GB (sufficient-by-design; no single entry may exceed it). TOC/entry counts
use `u32` with checked multiplication — overflow is `pack_invalid`.

Disk DTOs are SEPARATE from runtime asset structs: `ImageAsset`/`SceneAsset`
are runtime/owner types, not stable layouts, so `:serialize` partitions define
their own DTOs. `RelPtr`/`RelativeView` are IN-MEMORY views only: a bare
`RelPtr<T, i32>` is 4 B, `RelativeView` is 8 B (RelPtr i32 + u32 size). Disk
uses a dedicated `Rel32` (LE `u32` offset / `i32` signed form). Native
`Block`/`RelPtr` bytes are NEVER persisted (a `Block` in mapped bytes would
carry address-relative views); no Block serializer exists today —
`Core.Opaque.cppm` only has the in-memory Builder/sizeOf pattern used by
`Core.Logger.cpp`.

`construct()` in each `:serialize` partition runs a CHECKED offset/size/
alignment walk (footer → TOC bounds → entry core hash → per-offset
bounds/size/alignment, fail-closed) before building views into the pinned
mapping. Direct `->`/`[]` on mapped bytes is assert-or-UB, never a recovery
path. Mapping saves one memcpy + parse pass; it never performs hash
verification, decompression, page-fault handling, or TOC/footer checks.

Portability member rules: fixed-width ints only; no `bool` (use `u32`);
enum class with explicit `:u32`; `static_assert(sizeof==sum)` per struct, no
implicit padding; floats as `u32` bit-patterns; explicit alignment
attributes (not `#pragma pack`); versioning lives OUTSIDE mapped structs
(footer version + TOC kind/version packing).

Hash domains (three distinct concepts):

1. SOURCE hash — manifest/cook identity of source bytes + options.
2. PAYLOAD core-range hash — TOC integrity, covering only the entry's core
   region; dev string table/metadata live outside it.
3. GPU dedup hash — `image::contentHash(m_storage)` + full byte equality as
   implemented in `App.Renderer.GpuCaches.cpp:486-566`.

Collision rule: a hash only narrows candidates; the full byte compare decides.
`BindlessTextureCache::DedupKey` has NO `dimension` member despite the comment
claiming one — the spec follows the code.

Lifetime: v1 loads return independently owned bytes (option (a) — copy the
mapped core range into an owned `SharedBuffer`; image storage is already a
strong `SharedBuffer`, so the fix is small). The explorer's correction is
recorded: `moveToShared` is on `UniqueBuffer` (not `SharedBuffer`), and a
mapped `SharedBuffer` is OS-read-only but its owner flags lack the `immutable`
bit despite "read-only" comments — do not rely on immutability of mapped
views. An owning pack-storage mechanism (option (b)) is not used in v1.
Consequently `unmountAll()` cannot invalidate handed-off bytes.

## 6. Dev/shipping policy and cooker

- Dev: loose files under `getContentDir()`; pack optional.
- Shipping (`msvc-rel`, `BUILD_TESTING OFF`): pack required; missing/invalid =
  fatal, no silent fallback. The release-boot check is an explicit P2
  deliverable, not an assumed end state (the game still stages and loads loose
  shaders today).
- Cooker `cookPack()` runs on dev presets through the exact dev pipeline
  (`importAndConvert` → `decodeTo*` → `generateMipChain` → Slang per-target
  compile from one front-end), with one carve-out: blocked/embedded-chain
  images (KTX2 passthrough) reject CPU mip regeneration
  (`Image.Mips.cpp:189-197`). Dev/shipping parity for those images means the
  cooker stores the passthrough chain as decoded, and the runtime load path
  accepts the same chain without regeneration.
- No cooker disk cache (DDC) and no human-readable intermediate dumps in v1.

## 7. Shaders

`engine.shader` gains a narrow bytecode/reflection contract used by
`engine.asset`; it stays RHI-free, and pipelines/programs remain pass-owned:

- `loadModuleFromBytecode(container, reflection, out)` plus the existing
  source path for dev.
- `activeTarget()` query used by the cooker and the runtime loader.
- Container validation: DXIL container signature, SPIR-V magic `0x07230203`,
  target-must-match-active, on every load.
- `SharedModule` is a raw session-owned view (`Shader.cppm:72-102`,
  `Shader.cpp:233-244`); the service has no bytecode loader, no per-module
  unload, and no generation tracking today. Using a `SharedModule` after
  shader shutdown is a programmer lifetime bug (`PPR_ASSERT` in debug), not a
  recoverable error. A load issued after service shutdown returns
  `not_initialized` because `engine.asset` checks its OWN lifecycle flag
  BEFORE calling the shader loader; the existing loaders dereference
  `m_session` with no initialized check (`Shader.cpp:250-310`), so the
  `IShaderService` loaders must gain their own uninitialized precheck as part
  of this contract before the bytecode path ships.
- Session generation is added with the bytecode path so views can be
  generation-checked once the surface lands.
- Threading: shader loads stay on the serialized service-owner thread.
  `IShaderService` has unsynchronized session/target/`m_modules_loaded` state
  (`Shader.cpp:148-152,161-240,250-310`) and is NOT safe for worker-thread
  loads; off-thread shader loads are forbidden until an explicit shader
  concurrency contract lands.

## 8. Ownership and teardown

- Editor loads into its `LevelScope`, uploads at its discretion, waits for a
  REAL fence/host wait, then drops the CPU scope; level transition performs
  EXPLICIT scope release + cache reset after the wait (there is no tag-based
  GPU eviction; caches expose explicit release and reuse slots directly).
- The fence story is aspirational today (`unloadScene` releases without
  waiting; `Renderer::waitOnHost` is a queue wait). P3 must implement and
  assert the actual ordering: upload → fence/host wait → drop CPU scope →
  release/reset GPU entries.
- Shutdown: detach views/input → drop client scopes after fence → pass
  shutdown (caches → sampler → pipeline) → renderer waitOnHost → drop client
  shader scopes → `IShaderService::shutdown()` → `unmountAll()` →
  `service.shutdown()`. Every stage is fallible: each result is checked,
  the FIRST error is retained, and cleanup continues best-effort. Render-
  thread confinement applies to upload only; off-thread mutation →
  `operation_not_permitted`.
- Unmount may never overlap an in-flight load. v1 loads are synchronous and
  externally serialized, so shutdown is externally quiescent: the caller
  first prevents new service calls, then the service releases mapping/state
  and returns the first cleanup error.

v1 is synchronous-only by design. Async is deferred because shipping pack
loads have no import, decode, or compile work left (validation + hash +
checked construct + copy): an executor/future subsystem would add lifecycle
and completion complexity with no measured win. Only dev loose-source loading
is a clear future beneficiary. When async lands it must respect: a
caller-supplied executor; a per-job injected allocator (never share a mutable
`Arena`/`ScopedArena`/`Slab` across workers — decode/mip APIs need an
allocator seam first); caller-created contexts with an injected
`TimerManager&` for deadlines (never call `mainTimer()` from `engine.asset`);
and COOPERATIVE cancellation only at phase boundaries, because it cannot
interrupt Mango/STB/hashing/copying/Slang.

## 9. Errors and style

`Expected<T>` / `error_code`; `ContentErrc`: `not_found`, `pack_invalid`,
`version_mismatch`, `import_failed`, `decode_failed`, `shader_failed`,
`not_initialized`, `operation_not_permitted`. Style: `[[nodiscard]]`,
`not`/`and`/`or`, typed IDs only, `#if PPR_ENABLE_*`.

## 10. v1 non-goals

No generic importer/processor extensibility; no arbitrary options beyond the
typed per-type structs; no cooker disk cache or intermediate dumps; no
compression fields; no audio; no mount priority / patch / DLC layering; no tag
filtering beyond the single base pack; no opaque metadata region (no runtime
consumer, no writer, and hash-exclusion is unsafe without a semantic proof —
it may return only with a named consumer, a versioned codec, and either
inclusion in the hashed core region or a proven non-semantic rule); no weak
accelerator in v1 (optional hardening only, redefined as a decoded-result
cache keyed by asset type + source hash + usage + decode/mip options, with an
explicit concurrency contract and full-compare collision checks); no asset
base class and no wrapper template; no async service calls in v1 (see §8 for
the deferral rationale and the future requirements).

Future-notes (recorded so an async phase does not rediscover them): channel
type feasibility is mixed. `Channel<Expected<ImageAsset>>` and
`Channel<Expected<mesh::SceneAsset>>` are type-legal (`Array` is
`std::vector` with stateless `STL`; `SharedBuffer` and
`Expected` = `std::expected<T, error_code>` are nothrow-move and
nothrow-destructible), but `Channel<RelativeView<T>>` is NOT (`RelativeView`
contains `RelPtr`, whose move constructor is deleted —
`Core.Containers.cppm:661-691,1039-1046`), and shipping `SharedModule` through
a channel is type-legal but lifetime-unsafe.

## 11. Mango VFS verdict

REJECTED for runtime: no hash verification, silent footer failures,
whole-entry map only, exception-based errors, global registry, PRIVATE-dep
discipline break, unpinned fork. Keep the PPR-owned pack; steal the footer
shape/flags/zero-copy-split ideas. Optional host-only `.ppak` mapper seam for
tooling only.

Rejected: service-owned handles/receipts. Service-side ownership bought
nothing but bookkeeping: `SharedBuffer` already shares bytes identically
dev/shipping, and the GPU `BindlessTextureCache` already dedups by
content-hash and pins `SharedBuffer m_pinned` — yet the handle model put the
caller in charge of a lifetime it could not observe, created a dangling-
pointer class every receipt crossed, and forced weak_ptr-style silence-vs-
order tradeoffs on every lookup. Caller-owned handoff deletes the class
instead of managing it.

IUploadSink stays REMOVED: return values already cross the module boundary
with zero coupling; a sink would bake today's GPU consumers into the content
module so every new asset type would extend the service — backwards. A sink
only earns its keep for service-driven uploads (background streaming), which
is out of scope (synchronous manager, render-thread confined).

## 12. Tests and phases

Test tiers: a CPU-only wire-test target (modeled on the `ppr_asset_tier`
helper shape in `lib/engine/tests/asset/CMakeLists.txt:1-112`) for manifest
and pack wire tests, plus the existing GPU-coupled `engine.tests.asset` for
render-path tests.

CPU wire tests: manifest parse/validation failures (duplicate names, unknown
keys, dangling deps, version skew, tag coverage), pack footer/TOC corruption
(bad magic, truncation, bad offsets/alignment, overflow, hash mismatch,
target mismatch), metadata non-goal, ownership (returned bytes outlive
`unmountAll`; scope drop after fence), externally-serialized lifecycle
(contract test: concurrent service calls are rejected/serialized by the
caller contract, same-out races are documented misuse), load-after-shutdown
returning `not_initialized` without touching the shader session, unmount not
racing a load (single-threaded ordering assertion), `mountPack` failure
surfacing its `error_code` for a missing pack, and shutdown returning the first
cleanup error. GPU tier: dedup + pin behavior, explicit release/reset,
hash-collision tests, and the release boot with no loose assets staged.

- P0: contract/ownership — partitions, service lifecycle (`initialize` +
  `shutdown` with error returns), manifest root + typed options, v1 wire
  tables, CPU test target. Exit: reviewed ABI, no dangling owner, documented
  headless behavior.
- P1: CPU pack v1 — DTOs, checked construct, footer/TOC/hash, cooker host
  target, dev loose path. Exit: golden roundtrip, malformed/overflow tests,
  pack-only CPU loads.
- P2: shader + release integration — target subtable, bytecode/reflection
  API, magic/ABI validation, game client integration, staging. Exit:
  `msvc-rel` `app.game` boots with no `assets/` directory; missing/corrupt
  pack fails closed.
- P3: client/GPU lifetime — caller scopes, real fence/wait, pin/dedup,
  explicit release/reset. Exit: ASan ordering/teardown + hash-collision tests.
- P4: optional hardening — weak accelerator only if measured, migration
  policy, fuzz, Vulkan CI, full matrix (`msvc-dev`/`msvc-live`/`msvc-rel`,
  shuffled suites, code-reviewer verdict via the validation skill).

## 13. Bibliography

XNA:

- MonoGame content pipeline architecture —
  https://docs.monogame.net/articles/getting_to_know/whatis/content_pipeline/CP_Architecture.html
- XNA content pipeline overview —
  https://learn.microsoft.com/en-us/archive/technet-wiki/2231.xna-content-pipeline-overview
- Hargreaves, XML and the content pipeline —
  https://shawnhargreaves.com/blog/xml-and-the-content-pipeline.html
- Hargreaves, Building worlds with the content pipeline (slides) —
  https://shawnhargreaves.com/BuildingWorldsWithTheContentPipeline.pdf
- MonoGame ContentManager API —
  https://docs.monogame.net/api/Microsoft.Xna.Framework.Content.ContentManager.html
- K-State, Extending the pipeline —
  https://textbooks.cs.ksu.edu/cis580/11-content-pipeline/03-extending-the-pipeline/

Packs:

- Unreal PakFile API —
  https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/PakFile
- Unreal PakFile source walkthrough — https://en.imzlp.com/posts/12188/
- Unreal IoStore limits discussion —
  https://forums.unrealengine.com/t/cant-load-pak-files-at-runtime-with-iostore-option/2231691
- Unity, building AssetBundles —
  https://docs.unity.com/en-us/engine/6000.3/manual/assets-and-media/assets-managing-runtime/assetbundles-section/creating/asset-bundles-building
- Unity Addressables memory management —
  https://docs.unity3d.com/Packages/com.unity.addressables@1.14/manual/MemoryManagement.html
- Godot, exporting PCKs —
  https://docs.godotengine.org/en/stable/tutorials/export/exporting_pcks.html
- Godot `file_access_pack.cpp` source —
  https://github.com/godotengine/godot/blob/master/core/io/file_access_pack.cpp

Shaders:

- Slang, compiling shaders (user guide) —
  http://shader-slang.org/slang/user-guide/compiling
- Slang compilation API — https://shader-slang.org/docs/compilation-api/
- DirectXShaderCompiler — https://github.com/microsoft/DirectXShaderCompiler

Lifetime:

- Unreal derived data caches (DDCs) —
  https://dev.epicgames.com/community/learning/tutorials/bL60/unreal-engine-derived-data-caches-ddcs
- OGRE resources and resource managers — https://wiki.ogre3d.org/Resources+and+ResourceManagers
- OGRE resource management API —
  https://ogrecave.github.io/ogre/api/13/_resource-_management.html

CPU-GPU split:

- bgfx API reference — https://deepwiki.com/bkaradzic/bgfx/2.1-api-reference
- Learning Vulkan lesson — https://edw.is/learning-vulkan/
