# lib/engine/

## Responsibility and dependencies

Ten production modules form an acyclic graph, not a linear chain. These engine dependencies are registered by each module's `CMakeLists.txt`:

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
| [app](app/codemap.md) | Lifecycle, platform, input/player, camera, UI, renderer | core, math, shader, rhi, image, mesh |

Sim and physics do not import each other. App links neither; the game combines them and translates simulation state into owned render payloads. `import engine.app` does not expose all engine modules.

## Runtime navigation

`Application` owns platform/services, `TimerManager`, renderer and the optional render lane. `ApplicationEditor` owns client objects and pass/UI resources. Interactive rendering defaults to the worker; headless rendering stays inline. Both consume owned frame inputs. See [app](app/codemap.md) for admission, errors, display transactions, and teardown; avoid reconstructing those contracts from historical plans.

`Application::run()` re-arms its per-run state; it is not guarded by a permanent single-use application latch. A `RenderLane` instance itself is single-use and is recreated for a new run.

Module interfaces live in `.cppm`; non-inline/non-template definitions live in `.cpp`. Umbrella interfaces re-export their partitions. `CMakeLists.txt` adds each production directory and adds `tests/` unless `BUILD_TESTING` is explicitly OFF.

## Tests

Engine test executables are core, sim, app, and asset, using the shared static `engine.tests` library. Game configuration tests live under `game/` so engine tests never depend upward on game modules. See the [root test map](../../codemap.md#test-infrastructure) and owning CMake files for the current twelve-entry matrix.
