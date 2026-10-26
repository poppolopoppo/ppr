# lib/engine/tests/asset/

## Responsibility

`engine.tests.asset` is the executable behind the `engine.tests.asset.cpu`, `engine.tests.asset.gpu`, and `engine.tests.asset.gpu_validation` CTest entries. It covers CPU image/mesh processing, resilience and logging, plus shared GPU render/cache tests and private-app quarantine/lifecycle tests. Current group registration lives in `Asset.Tests.cpp`; avoid copying leaf counts into navigation docs.

## Registration and fixture lifetime

`Asset.Tests.cppm` exports a declaration-only `extern const UnitTest asset`. Implementation units expose private `Tests()` accessors, assembled by `Asset.Tests.cpp`; `main.cpp` uses the shared engine test runner. Test assertions use `PPR_TEST_ASSERT`.

The root first runs CPU image, mesh, resilience and observe groups. The `asset/render` group acquires a shared GPU fixture for cache, gate, draw and shared-mesh groups, releases it, then runs quarantine tests using private applications. CPU-only filtered runs never acquire that shared device. Fixture scope is recreated per runner loop.

`CMakeLists.txt` registers image/mip, mesh/UV-normal, render/cache/gate/draw/shared-mesh, resilience/observe and GPU fixture implementations. Its always-run `engine.tests.asset.assets` prerequisite stages textures, optional meshes and shaders beside the executable independently of relinking, restoring missing files and pruning stale files. Test-only Mango/GLFW dependencies do not alter production module boundaries.

## Running

The CPU and GPU entries use `--loop ${PPR_TEST_LOOPS}`, default 1, and TIMEOUT 450. `engine.tests.asset.cpu` runs `asset` with `--skip-test asset/render`; `engine.tests.asset.gpu` runs `asset/render` under `RESOURCE_LOCK gpu`. ASan does not select three loops automatically. Configure `PPR_TEST_LOOPS=3` for stress and restore 1 afterward.

Run both routine entries with `ctest --test-dir out/build/<preset> -L engine.tests.asset --output-on-failure -LE gpu_validation`. The opt-in `engine.tests.asset.gpu_validation` entry sets `PPR_TEST_GPU_VALIDATION=1` and runs the representative grid visibility gate with the RHI debug layers; run it separately with `-L gpu_validation`. For focused groups, invoke the executable with the shared runner's `--run-test`/`--skip-test` filters matching current group paths; run from its staged executable directory. GPU groups require the documented platform/GPU environment.

See [root test policy](../../../../codemap.md#test-infrastructure) for machine exclusivity, required asset gates and loop selection.

For explicit Vulkan readbacks, set the test-only environment variable
`PPR_ASSET_TEST_VULKAN=1` in the serialized build shell, invoke the existing
`asset/render/gates` filters in a fresh process, then clear the variable. The
shared fixture initializes Vulkan before booting its headless application and
asserts the actual device type. The ordinary invocation keeps its existing default
backend. Invalid values and initialization/readback failures fail the tests;
the runner has no skip result, so unavailable Vulkan/bindless support must be
reported separately rather than counted as passing coverage. Private-application
quarantine tests do not use this shared-fixture override.

Grid reference/readback fixtures prepare bounded cell cohorts with
`tryPrepareCellUploads`, refreshing renderer fence completion before retries.
They retain content/pixel and candidate-retirement assertions; the removed
queue-idle convenience API is not a separate reference publication path.
