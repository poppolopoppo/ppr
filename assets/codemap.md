# assets/

## Responsibility

Runtime asset root for the demo. Currently shader-only: holds the Slang sources that `engine.shader` compiles at
startup and `engine.rhi` links into pipelines. No textures, meshes, or config files live here yet.

## Design

- Single subtree `assets/shaders/` with plain-text `.slang` files (row-major session layout, see `engine.shader`).
- `game/CMakeLists.txt` POST_BUILD copies the whole `assets/shaders` directory → `<exe-dir>/shaders/`; the exe never
  reads from the source tree at runtime.
- ImGui overlay shader is NOT an asset — it is an embedded source string in `lib/engine/app/ui/App.UI.ImGui.cpp`.

## Flow

`game/CMakeLists.txt` POST_BUILD (`copy_directory assets/shaders → <exe>/shaders`) → at startup
`IShaderService::loadModuleFromFile` → `io::mapFile` → Slang compile → RHI pipeline creation.

## Integration

- **Consumers**: `engine.shader` (compilation), `engine.rhi` / `Renderer` + `TrianglePass` (pipelines), `game` demo.
- **Depends on**: Slang toolchain (compile-time of the asset at runtime).
- **Provides**: on-disk shader sources; deployed copy next to `app.game`.

## Key Files

- `shaders/` — Slang sources (see `assets/shaders/codemap.md`).
