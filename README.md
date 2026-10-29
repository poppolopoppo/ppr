# PPR Game Engine

PPR is a real-time C++23 engine built with C++20 modules. Nine engine libraries support an interactive colony demo, headless GPU checks, and deterministic simulation. The engine is the foundation; the demo contains game-specific agents, pathfinding, digging, and building rather than a finished game.

## Start here

- [AGENTS.md](AGENTS.md) is the engineering contract.
- [codemap.md](codemap.md) locates current subsystems; source and CMake registration establish implemented behavior.
- [docs/README.md](docs/README.md) distinguishes current reference material from proposals and historical evidence.
- Operational procedures live in [.opencode/skills/](.opencode/skills/), especially [build-system](.opencode/skills/build-system/SKILL.md) and [validation](.opencode/skills/validation/SKILL.md).

## Architecture

| Module | Responsibility | Engine dependencies |
|---|---|---|
| `engine.core` | Types, memory, containers, concurrency, I/O, configuration, services, HAL | Foundation |
| `engine.math` | Mango math and engine math helpers | core |
| `engine.shader` | Slang compilation | core |
| `engine.rhi` | Slang-RHI abstraction and projection helpers | core, math, shader |
| `engine.image` | CPU image decoding and processing | core, math |
| `engine.mesh` | CPU static mesh import | core, math |
| `engine.sim` | Chunk grid, ECS, fixed timestep, snapshots, world generation | core, math |
| `engine.physics` | Box2D scene, chunk colliders, coupling | core, math; private Box2D |
| `engine.app` | Application lifecycle, platform, input, camera, UI, rendering | core, math, shader, rhi, image, mesh |

Dependencies point down this graph. Simulation and physics are siblings; `engine.app` links neither. The game owns their coordination and links physics privately. See [engine map](lib/engine/codemap.md) and the owning `CMakeLists.txt` files for public/private external dependency details.

`Application` owns the run loop, platform/services, renderer, and render worker. `ApplicationEditor` owns the interactive window, input, player, camera, viewport, render passes, and UI. Client ownership remains explicit even when GPU work executes on the worker.

The interactive path defaults to a dedicated render lane. The main thread pumps events, updates simulation and UI, and publishes owned camera/window/pass/UI payloads. The worker prepares GPU resources, encodes, submits, and presents. Headless applications use the inline path. Frame admission is bounded; busy admission preserves elapsed simulation time and keeps collecting native events. GPU completion, CPU payload consumption, and presentation are distinct milestones. See [application](lib/engine/app/codemap.md) and [frame](lib/engine/app/frame/codemap.md) maps.

The renderer owns device-facing orchestration. Each pass owns its pipelines, buffers, and content encoding. Grid tiles use `grid_tiles.slang`, generated debug geometry uses `mesh_bindless.slang`, and ImGui supplies the UI path. Drawing consumes immutable camera snapshots with row-major, row-vector, left-handed, +Z-forward, +Y-up matrices and [0,1] depth.

Recoverable operational failures use `std::error_code` / `std::expected`. Teardown detaches producers, settles work, then releases dependent resources while retaining the first error. GPU teardown requires proven idle or confirmed device removal; unknown completion takes the application's fail-stop path before resource release.

## Build

The checked-in configuration requires CMake 4.3+, Ninja, Git, and a toolchain supporting C++23 modules and `import std`. MSVC presets require an activated x64 VS 18 Insiders environment with MSVC 14.51+; [MSVCToolchain.cmake](cmake/MSVCToolchain.cmake) enforces this. Clang builds also require a compatible standard library/module toolchain. Hidden `gcc-*` presets are explicitly unsupported for modules.

Windows: keep one developer shell for configure, build, and test:

```bat
call "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat"
cmake --preset msvc-dev
cmake --build out/build/msvc-dev --parallel
ctest --preset msvc-dev
```

Linux uses Clang/libc++ with matching module tooling and `VCPKG_ROOT` pointing to vcpkg. The Clang presets select the repository's `x64-linux-libcxx` overlay triplet. First-time dependency setup needs network access. See [build-system](.opencode/skills/build-system/SKILL.md) for prerequisites and [cmake/codemap.md](cmake/codemap.md) for dependency/backend details. macOS needs separate toolchain/triplet verification; these Linux triplet settings are not a macOS setup.

```sh
cmake --preset clang-dev
cmake --build --preset clang-dev --target engine.tests.core
ctest --preset clang-dev -R '^engine[.]tests[.]core$'
```

The example verifies core only. Build presets support both platforms; test presets provide routine and GPU-validation selections. Select the task's required targets and tests under [validation](.opencode/skills/validation/SKILL.md).

`msvc-dev` enables developer diagnostics and ASan. `msvc-live` enables Debug Edit & Continue and disables sanitizers/cache. `msvc-rel` is optimized with tests disabled; `msvc-profile` inherits it and enables profiling markers plus EMPHASIS logging. Other release presets do not automatically disable tests. See [CMakePresets.json](CMakePresets.json).

Windows and Linux have jobs in [.github/workflows/ci.yml](.github/workflows/ci.yml); macOS has a HAL and Clang presets but no CI job. Workflow presence does not establish a currently passing build, and its Windows environment activation must satisfy the stricter MSVC guard.

## Demo

On Linux, run `./out/build/clang-dev/game/app.game --help` after building
`app.game`; executable names have no `.exe` suffix. Run from the generated game
directory when the invocation needs staged assets. Windows examples:

```bat
out\build\msvc-dev\game\app.game.exe --help
out\build\msvc-dev\game\app.game.exe
out\build\msvc-dev\game\app.game.exe --smoke
```

The interactive demo loads an authored, seeded 4096×4096-cell world at three fine cells per metre and starts one directly controlled dupe. It preserves autonomous-agent pathfinding and provides dig/build/demolish tools, inspection, camera controls, and Colony/Performance/Display UI. Terrain uses catalog textures and a shared contour through GPU visibility and indirect drawing. It does not load a committed Kenney fixture scene. Assets are staged beside the executable by `game/CMakeLists.txt`.

`--smoke` runs offscreen GPU readback and simulation checks. For external profiling, `--profile100` selects a 100-agent fixture and `--exit-after S` bounds the run. Bespoke CSV timing, scripted profiling controls, load/time-trial commands, and game-local exception interception have been retired. Startup/profile/display options are resolved through typed configuration; `--help` is generated from the current declarations. See [game/codemap.md](game/codemap.md) rather than copying an old flag list.

For agent-driven interactive debugging, [game-control](.opencode/skills/game-control/SKILL.md) documents the installed Computer Use plugin and an external Windows controller fallback for process targeting, screenshots, keyboard, mouse, wheel, and normal shutdown. Semantic engine-state access remains future MCP work.

## Tests

With tests enabled, current CMake registers twelve entries named `<target>[.<qualifier>]` by the shared `ppr_add_test_entry` helper (`cmake/Tests.cmake`):

| Entry | Coverage |
|---|---|
| `engine.tests.core` | Foundation, including configuration; GLFW-free |
| `engine.tests.sim` | Grid, timestep, snapshots, ECS, world generation, physics; GLFW-free |
| `engine.tests.app.death_lifecycle` | Death/lifecycle and fast run gates |
| `engine.tests.app.slow_gates` | Slow lifecycle/admission gates |
| `engine.tests.app.display_window` | Display/window transactions |
| `engine.tests.app.functional_groups` | Input/player/camera, packets, pacing, grid and display groups |
| `engine.tests.app.frame_fence` | Frame fences |
| `engine.tests.app.services_io` | Services, shader, renderer/UI lifecycle, render lane and window input |
| `engine.tests.asset` | Image/mesh processing, GPU caches and render integration |
| `game.tests` | Game invocation and display model/persistence |
| `app.game.worldgen_frontier` | CPU-only shipped Frontier pack load/compile validation |
| `app.game.worldgen_hollows` | CPU-only shipped Hollows pack load/compile validation |

All six app shards and the asset suite have `RUN_SERIAL TRUE`; `-j4` respects those properties. Keep independent matrix invocations exclusive on one machine. App/GPU tests need the platform/GPU facilities documented in their owning CMake files; Linux CI runs under Xvfb with Mesa.

`PPR_TEST_LOOPS` defaults to **1**, including ASan builds. Configure with `-DPPR_TEST_LOOPS=3` for stress, then explicitly restore `1` when reusing that build tree. Focused runs use `ctest --test-dir out/build/<preset> -R <ctest-regex> --output-on-failure` with the entry names above (dots are regex metacharacters — quote and escape them, e.g. `-R "engine\.tests\.app\.frame_fence"`). Counts and filters are defined in [app tests](lib/engine/tests/app/CMakeLists.txt), [engine tests](lib/engine/tests/CMakeLists.txt), and [game](game/CMakeLists.txt).

## Scope and contribution

Implemented foundations include fixed-step simulation, a dedicated render lane, pacing, typed configuration, CPU assets, and pass-owned rendering. Cooked asset packages, advanced shading, full editor tooling, and the proposed particle/clone designs are not established by those foundations. Plans are design input, not proof of implementation or permission to expand a task.

Keep changes focused; follow the engineering contract and the owning subsystem map. Behavioral changes require proportionate tests. C++ formatting and semantic checks use the repository scripts, not hand formatting.

MIT license: [LICENSE](LICENSE).
