# assets/

## Responsibility

Runtime asset root for the demo. Shader sources that `engine.shader` compiles at startup and `engine.rhi` links into pipelines live in `shaders/`; `textures/` holds shared runtime textures; `meshes/` contains static GLB fixtures, including 16 staged Kenney colony assets under `meshes/kenney_colony/`. Shader, texture, and mesh trees stage beside executables on every explicit consumer build.

## Design

- `meshes/kenney_colony/` preserves the upstream pack-relative `Models/<format>/` layout, required texture siblings, per-pack licenses, and `PROVENANCE.md`; runtime code resolves it only below `getContentDir() / "meshes"`.
- `game/CMakeLists.txt` synchronizes `assets/shaders`, `assets/textures`, and (when present) `assets/meshes` beside the executable; runtime never reads these trees from the source checkout.
- ImGui overlay shader is NOT an asset — it is an embedded source string in `lib/engine/app/ui/App.UI.ImGui.cpp`.

## Flow

`game/CMakeLists.txt` synchronizes the shader, texture, and mesh trees beside the executable. At startup `IShaderService::loadModuleFromFile` compiles the shaders, then the game fixture imports `meshes/kenney_colony/*.glb`, decodes referenced images, and uploads the scenes to GPU caches.

## Integration

- **Consumers**: `engine.shader` (compilation), `engine.rhi` / `Renderer` + `TrianglePass` (pipelines), `game` demo.
- **Depends on**: Slang toolchain (compile-time of the asset at runtime).
- **Provides**: on-disk shader sources; deployed copy next to `app.game`.

## Key Files

- `shaders/` — Slang sources (see `assets/shaders/codemap.md`).
- `textures/` — shared runtime textures (see `textures/PROVENANCE.md`; no binary fixtures committed yet).
- `meshes/` — static GLB/gltf fixtures, including 16 staged Kenney colony assets under `meshes/kenney_colony/`.

worldgen/default contains the versioned 63-material, eleven-biome, 26-template, two-profile PPR pack, with 44 named fields and 134 texture maps (126 material albedo/normal maps and eight background maps). game/CMakeLists.txt stages this tree on every explicit game/test build, including data-only edits/deletions; CPU --validate-worldgen validates the deployed copy.
