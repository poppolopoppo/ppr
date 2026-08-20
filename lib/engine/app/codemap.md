# lib/engine/app

## Responsibility
The `engine.app` application layer module serves as the umbrella header that re-exports all submodules composing the application layer. It provides the top-level import point for consumers who need the full application stack — from core types and RHI to input, window, player, UI, and camera subsystems. This module is the primary include for `game/main.cpp` and any code that needs to reference engine.app types.

## Design
- Single module unit (`App.cppm`) that `export import`s all sub-partitions via `export import :<submodule>;`
- Re-exports five engine core modules: `engine.core`, `engine.math`, `engine.rhi`, `engine.shader`
- Re-exports application-internal modules: `application`, `camera`, `input.*`, `player.*`, `renderer`, `service.*`, `viewport`, `window.*`, `ui.imgui`
- Follows the PPR module convention: `.cppm` = interface (exports), `.cpp` = implementation (definitions)
- Uses `export import` (not `import`) to make submodules available to downstream consumers

## Flow
- Application entry point (`game/main.cpp`) begins with `import engine.app;`
- All engine modules are pulled in transitively through this single umbrella
- No runtime flow — this is a compile-time dependency aggregation layer

## Integration
- **Consumers**: `game/main.cpp`, test targets (`EngineCoreTests`, `EngineAppTests`)
- **Depends on**: `engine.core`, `engine.math`, `engine.rhi`, `engine.shader` (lower-level engine modules)
- **Provides**: All `engine.app:*` module namespaces for downstream use

## Key Files
- `App.cppm` — umbrella module that re-exports all application submodules
- `App.Application.cppm` — interface for `pP::Application` class (main loop, service store, directory resolution)
- `App.Application.cpp` — implementation of application lifecycle (initialize/update/render/terminate)
- `App.TemplateInstantiations.cpp` — compiler-generated template instantiations