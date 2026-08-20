# lib/engine/

## Responsibility
The PPR engine library tree. Hosts the five C++20 module libraries that compose the engine, organized by dependency layer: `engine.core` (foundation) → `engine.math` (vector math) → `engine.shader` (Slang compilation) → `engine.rhi` (GPU abstraction) → `engine.app` (application layer).

## Design
- **Layered dependency chain**: `engine.app` depends on all four lower modules; `engine.rhi` depends on `engine.shader` and `engine.math`; `engine.shader` depends on `engine.core`; `engine.math` depends on `engine.core`.
- **Module partition convention**: `.cppm` = interface (exports), `.cpp` = implementation; partitions named `engine.<lib>:<partition>`; umbrellas re-export via `export import :partition;`.
- **Service locator**: `IService` → `ServicesStore` (parent-chain fallback) → `ServiceInjector`; per-viewport child stores chain to the root.
- **Allocator hierarchy**: GPA → OS → PagePool → HugePage/SmallPage → Arena/ScratchPad → composite (InSitu, Fallback, Threshold, Pooling, LocalCache).
- **Matrix convention**: row-major, row-vector `mul(float4, matrix)`; `EProjectionConvention` handles D3D vs Vulkan Z-ranges.

## Flow
`game/main.cpp` → `import engine.app` → umbrella pulls in all five modules transitively → `pP::Application` constructs → resolves directories → discovers services (input, window, player, RHI, shader) → per-frame update/render loop.

## Integration
- Consumed by: `game/main.cpp`, test targets (`EngineCoreTests`, `EngineAppTests`).
- Depends on: third-party libraries (Slang, Slang-RHI, mango::math, GLFW, DearImGui, rapidhash, STB) via `cmake/external/`.

## Key Files
- `core/` — foundation library (types, memory, containers, concurrency, IO, HAL). See [core/codemap.md](core/codemap.md).
- `math/` — vector math wrapping mango::math. See [math/codemap.md](math/codemap.md).
- `shader/` — Slang compilation service. See [shader/codemap.md](shader/codemap.md).
- `rhi/` — GPU abstraction wrapping Slang-RHI. See [rhi/codemap.md](rhi/codemap.md).
- `app/` — application layer (Application, input, window, player, renderer, UI, platform). See [app/codemap.md](app/codemap.md).
