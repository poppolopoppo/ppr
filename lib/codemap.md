# lib/

## Responsibility
Source root for the PPR engine libraries. All engine code lives under `lib/engine/` as C++20 modules. The `lib/` directory also hosts the test infrastructure (`lib/engine/tests/`, excluded from codemap per skill rules).

## Design
- Single library tree: `lib/engine/` contains the five engine modules (`engine.core`, `engine.math`, `engine.shader`, `engine.rhi`, `engine.app`) plus the test suite.
- Module partition convention: `.cppm` = interface (exports), `.cpp` = implementation; partitions named `engine.<lib>:<partition>`; umbrellas re-export via `export import :partition;`.
- Tests are in `lib/engine/tests/` (excluded from this codemap) and split into `EngineCoreTests` (GLFW-free) and `EngineAppTests` (GLFW-linked).

## Flow
`game/main.cpp` → `import engine.app` → umbrella re-exports `engine.core`, `engine.math`, `engine.rhi`, `engine.shader` → transitive access to all partitions.

## Integration
- Consumed by: `game/` (entry point), CMake build system (`cmake/`).
- See [engine/](engine/codemap.md) for the full engine module map.

## Key Files
- `engine/` — all engine module libraries (see [engine/codemap.md](engine/codemap.md)).
