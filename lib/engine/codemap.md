# lib/engine/

## Responsibility and dependencies

Eleven production modules form an acyclic graph, not a linear chain. These engine dependencies are registered by each module's `CMakeLists.txt`:

| Module | Responsibility | Engine dependencies |
|---|---|---|
| [core](core/codemap.md) | Types, allocators, containers, concurrency, I/O, configuration, services, HAL | none |
| [asset](asset/codemap.md) | Named pack indices, typed descriptor importing, shared asynchronous preparation | core |
| [math](math/codemap.md) | Mango math and helpers | core |
| [shader](shader/codemap.md) | Slang shader service | core |
| [rhi](rhi/codemap.md) | Slang-RHI and projection helpers | core, math, shader |
| [image](image/codemap.md) | CPU image assets and named texture processing | core, math, asset |
| [mesh](mesh/codemap.md) | CPU static mesh assets | core, math |
| [sim](sim/codemap.md) | Chunk grid, ECS, footprint vocabulary, fixed timestep, snapshots, worldgen descriptors and lowering | core, math, asset |
| [physics](physics/codemap.md) | Scene, chunk colliders, coupling; private Box2D | core, math |
| [matter](matter/codemap.md) | Direct pinned PB-MPM GPU solver: split hot/cold device rows, checked transfers, asynchronous working batches, explicit publication | core, math, shader, rhi |
| [app](app/codemap.md) | Lifecycle, platform, input/player, camera, UI, renderer | core, math, shader, rhi, image, mesh |

Sim and physics do not import each other. App links neither; the game combines them and translates simulation state into owned render payloads. `import engine.app` does not expose all engine modules.

## Runtime navigation

`Application` owns platform/services, `TimerManager`, renderer and the optional render lane. `ApplicationEditor` owns client objects and pass/UI resources. Interactive rendering defaults to the worker; headless rendering stays inline. Both consume owned frame inputs. See [app](app/codemap.md) for admission, errors, display transactions, and teardown; avoid reconstructing those contracts from historical plans.

`Application::run()` re-arms its per-run state; it is not guarded by a permanent single-use application latch. A `RenderLane` instance itself is single-use and is recreated for a new run.

Module interfaces live in `.cppm`; non-inline/non-template definitions live in `.cpp`. Umbrella interfaces re-export their partitions. `CMakeLists.txt` adds each production directory and adds `tests/` unless `BUILD_TESTING` is explicitly OFF.

## Tests

Engine test executables are core, sim, app, and asset, using the shared static `engine.tests` library. GPU matter fixtures live in the app tier (`matter_gpuTests()`); `engine.tests.sim` no longer links `engine.matter`. Game configuration tests live under `game/` so engine tests never depend upward on game modules. See the [root test map](../../codemap.md#test-infrastructure) and owning CMake files for the current twelve-entry matrix.

Resident matter fixtures in the app tier use compact completion followed by
explicit committed snapshots for numerical observations. Inline game checkpoint
fixtures drive the same owned Runtime requests; no synchronous/nonresident
Runtime path is maintained. Existing transfer/contact/pool/dam workloads and
registered deadlines remain unchanged by this convergence.

Render-lane lifecycle fixtures exercise ticket-only bounded waits on the existing
owned operation slot, including queued/running cancellation and real-policy timeout
capture retention. Grid fixtures and headless capture share bounded cell publication;
packetless renderer surface conveniences remain supported.

App payload tests use the private `game/colony/Colony.AuthoredFixture.Tests.h`
fixture with real authored compilation and zero generated moving/gas rows. The
seed-only generator is no longer compiled into engine.tests.app.

## Pinned-fork native conformance gate

`tests/app/App.MatterFork.Tests.cpp` owns the private direct-RHI fork fixture;
`App.MatterFork.Oracle.h` contains frozen actual-WGSL state words, with provenance
in the recovery diagnostic manifest. The dedicated `matter_fork_gpu` shard
selects two leaves for stage/tick/semantic-restart equality and checked-scatter
rejection. This is a conformance prerequisite; production solver ownership,
Runtime/renderer transport and circle presentation remain unchanged/pending.
