# lib/engine/

## Responsibility

The PPR engine library tree. Hosts the five C++20 module libraries that compose the engine, organized by
dependency layer: `engine.core` (foundation) → `engine.math` (vector math) → `engine.shader` (Slang
compilation) → `engine.rhi` (GPU abstraction) → `engine.app` (application layer).

## Design

- **Layered dependency chain** (mirrored in each `CMakeLists.txt` via `setup_ppr_project`): `engine.app`
  → core + math + shader + rhi (+ `glfw`, `mango` private, `imgui.base`/`imgui` public); `engine.rhi` → core +
  math + shader (public) + `slang-rhi`/`slang` (public); `engine.shader` → core (public) + `slang` (private);
  `engine.math` → core (public) + `mango` (private); `engine.core` → `rapidhash` (private) + per-platform HAL
  sources (`hal/${PPR_HAL_PLATFORM}/`, windows adds Random + RingBuffer).
- **Module convention**: `.cppm` = interface (exports), `.cpp` = implementation; single-module libraries
  (`math`, `shader`, `rhi`) vs partitioned umbrellas (`core`, `app`); implementations use `module engine.<lib>;`
  + `import :partition;`.
- **Service locator**: `IService` → `ServicesStore` (parent-chain fallback) → `ServiceInjector`; per-viewport
  child stores chain to the root.
- **Matrix convention**: Mango-native left-handed view space (+Z forward, +Y up), row-major, row-vector
  `mul(float4, matrix)`; Slang sessions fix `SLANG_MATRIX_LAYOUT_ROW_MAJOR`; RHI projections use
  `orthoD3D`/`perspectiveD3D` with [0,1] depth, shared untransposed by all backends.

## Flow

`game/main.cpp` (`app.game`: core + app + math + shader + rhi) → `import engine.app` pulls all five modules
transitively → `pP::Application` constructs → resolves directories → discovers services (input, window, player,
RHI, shader) → per-frame update/render loop; shutdown unwinds RHI then shader sessions.

## Integration

- Consumed by: `game/main.cpp`, test targets (`engine.tests.core`, `engine.tests.app`).
- Depends on: third-party libraries (Slang, Slang-RHI, mango::math, GLFW, DearImGui `imgui.base`/`imgui`,
  rapidhash, STB) via `cmake/external/` + `setup_ppr_project`.

## Key Files

- `core/` — foundation library (types, memory, containers, concurrency, IO, HAL).
  See [core/codemap.md](core/codemap.md).
- `math/` — mango re-export, constants, math helpers (`Math.cppm` only). See [math/codemap.md](math/codemap.md).
- `shader/` — Slang session + module loading. See [shader/codemap.md](shader/codemap.md).
- `rhi/` — GPU abstraction + projections + `IRhiService`. See [rhi/codemap.md](rhi/codemap.md).
- `app/` — application layer (Application, input, window, player, renderer, scene, UI, platform).
  See [app/codemap.md](app/codemap.md).
