# assets/

## Responsibility
Runtime asset root for the PPR engine demo. Currently holds the Slang shader sources consumed by `engine.shader` (loaded at startup via `IShaderService::loadModuleFromFile`).

## Design
- Flat asset tree under `assets/`; shader sources live in `assets/shaders/`.
- Shader files are plain `.slang` text, compiled at runtime by the Slang session (row-major matrix layout, see `engine.shader`).
- Copied into a `shaders/` subdirectory next to the `VideoGameApp` executable by the POST_BUILD step in `game/CMakeLists.txt`.

## Flow
`game/CMakeLists.txt` POST_BUILD copies `assets/shaders` → output dir → `IShaderService::loadModuleFromFile` reads them at runtime (`Renderer::initialize`).

## Integration
- Consumed by: `engine.shader` (`IShaderService`), `engine.rhi` (pipeline creation), `game` demo.
- Depends on: Slang compiler toolchain.

## Key Files
- `shaders/` — Slang shader sources (see `assets/shaders/codemap.md`).
