# lib/engine/tests/asset/

## Responsibility

`engine.tests.asset` — asset-pipeline suite (`EngineAssetUnitTests`,
`asset/` root with the same extern-umbrella + `Tests()`-accessor + POST_BUILD fixture-staging
pattern). Five tops, 62 leaves, no single-leaf groups (10 mesh/image-agnostic
proofs moved to engine.tests.core/app — see below):
`asset/image` (20: image RGBA/block decode, format predicates, block geometry,
content-hash, errc mapping, frozen cross-thread sharing via runtime-generated
PNG/JPG/DDS fixtures — no binaries committed — plus `asset/image/mips`
chain/coverage/bleed/quality goldens; the mapFile-missing errc and
path-separator proofs live in engine.tests.core),
`asset/mesh` (23: GLB-embed + external-URI import, verbatim LH values, MR split,
rejections, malformed-GLB fail-closed, 64 B layout, ID sentinels, analytic
tangent-w probe, plus `asset/mesh/uv` bakes/sets + inverse-transpose normals;
material pack factors, pipeline variants, and the draw plan contract live
in engine.tests.app),
`asset/render/caches` (9: upload/resolve/release, capacity telemetry,
overflow/wrong-thread fail-closed, scene coexistence, residency, cache lifecycle logs),
`asset/render/gates` (3: textured box, ORM goldens, tangent-w render proof),
`asset/render/draws` (5: payload-base binding, instancing differentiation, empty-scene +
shutdown idempotency, stale-handle and bucket-stride fail-closed through `render()`),
`asset/render/quarantine` (2: editor scene flow + device-loss restart, private apps),
`asset/resilience` (7: fuzz corpus, 8-way import/decode storm),
`asset/observe` (5: CPU-owned logging lines + helper; the Once helper proof
lives in engine.tests.core).

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

## Test tiers

- `EngineAssetUnitTests` is the full run (ASan 3-loop rigor, TIMEOUT 450);
  per-top CTest entries (`ppr_asset_tier`) run via `ctest -L <tier>`:
  `tier-cpu` (image/mesh/resilience/observe — hold no SharedGpu refs, never
  boot a device), `tier-gpu` (render/caches, render/gates, render/draws,
  render/quarantine). GPU tiers run `--loop 1` in-tier; the full run keeps the
  3-loop rigor. The runner loop lives in `engine.tests::runSuite` (shared
  infra, outside the tree), so the render scope re-boots once per `--loop`
  iteration — hoisting above the loop would require changing shared test
  infra and is intentionally not done. No tier aliases: the ora-1 names are gone.
- Sharing lifetime (three phases, Asset.Tests.cpp root):
  phase 1 recurses CPU-only tops holding no SharedGpu refs; phase 2 is the
  ender parent holding one `SharedGpu::acquire()` across caches + gates +
  draws (first acquire boots the headless app once per loop; teardown is
  inverse — per-leaf `TrianglePass::shutdown`, then the render release);
  phase 3 runs `render/quarantine` (editor flow with its private GateEditorApp,
  loss restart with its private LossTestApp — never shared refs, after the
  shared session is torn down).
  Lazy by construction: the `render` body only executes when the `--run-test`
  filter matches `asset/render`, so CPU-only filtered runs never boot a device
  (no filter plumbing in main.cpp or the fixture was needed).
- The loss leaf is the suite's sole pass-level shutdown + initialize reinit
  proof (cache-level reinit lives in `asset/render/caches`
  `cache_residency_and_composed_identity`). Both loss inits are full shader
  compiles by production design (`TrianglePass::initialize` always reloads
  `mesh_bindless.slang`; `m_caches_ready` is only set there, and in-place
  reinit is invalid), so the 2-init cycle stays; quarantine keeps it off the
  shared session.
- The caches overflow/wrong-thread leaves stay in the shared set: every
  failure path only asserts `error_code`s and releases what it acquired
  (overflow shuts down its small cache; wrong-thread retries nothing), so the
  shared device is net-unchanged after per-leaf shutdown.
- `asset/mesh/tangent_w_analytic_probe` (lane math + fixture tangent-unit
  invariant) and `asset/render/gates/tangent_w_render_proof`
  (distinctive-texel render proof) prove different levels — both stay.
- Vulkan CI needs no new build config: RHI maps DeviceType to the compile target
  at runtime and the suite is headless (windowless Application +
  renderToTexture/readback), so cpu tiers run GPU-free while gpu-tier leaves need
  a bindless-capable device (SwiftShader/Lavapipe or a GPU runner).
- errc mapping the suite pins: malformed/over-limit → `invalid_argument`,
  deferred content (blend, clearcoat/sheen/anisotropy, joints/skins) →
  `function_not_supported`, importer throws → `import_failed`. Deterministic
  rejection never grows a new errc.

## Integration

- Depends on: `engine.image`, `engine.mesh`, `engine.app`, `engine.rhi`, `engine.shader`,
  `engine.core`, `engine.math`, `engine.tests`.
- Staging: POST_BUILD `copy_directory assets/textures`, `assets/meshes`, `assets/shaders`
  beside the binary; CTest `EngineAssetUnitTests` runs in the binary dir.

## Key Files

- `Asset.Tests.cppm` — extern umbrella decl only.
- `Asset.Tests.cpp` — `asset` root definition (phase 1 CPU tops, phase 2
  `render` scope is bypassed by CPU-only filters, phase 3 quarantine).
- `Asset.GpuFixture.cpp` — refcounted shared headless app (`SharedGpu`).
- `Asset.Image.Tests.cpp` — `asset/image` image goldens (17 leaves).
- `Asset.Image.Mips.Tests.cpp` — `asset/image/mips` mip goldens (4 leaves, nested under image).
- `Asset.Search.Tests.cpp` — `asset/mesh` mesh import (13 leaves) + analytic probe.
- `Asset.Search.UvNormal.Tests.cpp` — `asset/mesh/uv` bakes (9 leaves, nested under mesh).
- `Asset.Render.Caches.Tests.cpp` — `asset/render/caches` (9 leaves, incl. lifecycle logs).
- `Asset.Render.Gates.Tests.cpp` — `asset/render/gates` (3 shared) + editor leaf for quarantine.
- `Asset.Render.Draws.Tests.cpp` — `asset/render/draws` (5 shared) + `asset/render/quarantine` parent.
- `Asset.Resilience.Tests.cpp` — `asset/resilience` fuzz + storm (7 leaves).
- `Asset.Observe.Tests.cpp` — `asset/observe` logging proofs (5 leaves).
- `main.cpp` — runner entry.
- `CMakeLists.txt` — target + CTest registration + staging.
- Cross-program homes (mesh/image-agnostic proofs, moved here from asset):
  `core/io` (mapFile-missing errc, path separator), `core/service`
  (Log::Once helper), `core/enums` (bindless budgets),
  `app/render_view` (material pack, pipeline variants, draw plan).
