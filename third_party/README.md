# Third-party dependencies

Every third-party library the PPR build fetches, links, or ships gets a folder
here. Each folder documents in its `README.md`: the upstream project, the exact
pin and how it is resolved, why that version, the license (with a preserved
correct notice + bytes/SHA-256), and the integration scope inside PPR. License
notices are preserved so redistributable builds have their required notices
in-tree instead of depending on build-tree artifacts (`out/`, `_deps/`,
`vcpkg_installed/`) that are intentionally outside the repository.

Sources of dependency versions:

- **vcpkg manifest** (`vcpkg.json`): `builtin-baseline`
  `544a4c5c297e60e4ac4a5a1810df66748d908869` (commit date 2025-12-16) plus
  explicit `overrides`. Versions below were read from the resolved install
  (`vcpkg list` against an installed manifest tree) on the survey date. A
  baseline bump is a dependency-resolution change for every preset and is its
  own reviewed change (see the FreeType entry for the M4 precedent).
- **CPM source packages** (`cmake/Dependencies.cmake` + `cmake/external/`):
  immutable pins and moving refs are recorded per entry.

## Survey (2026-10-18)

| Entry | Version at survey | Route | First-order PPR consumers |
|---|---|---|---|
| [Mango](Mango/README.md) | `main` @ `a091541` (2026-09-24) | CPM source build | `engine.math` (math re-export), `engine.image` (`mango-image`), `engine.mesh` (`mango-import3d`) |
| [rapidhash](rapidhash/README.md) | `master` @ `1ae7842` (2026-08-19) | CPM source | `engine.core` hashing |
| [Slang-RHI](SlangRHI/README.md) | `ad1b92f` = `ad1b92f16…` (#864, 2026-09-17) | CPM source build | `engine.rhi` |
| [Slang](Slang/README.md) | 2026.17.1 prebuilt | fetched by Slang-RHI (`SLANG_RHI_FETCH_SLANG`) | `engine.shader`, `engine.rhi`, render passes, game |
| [STB](STB/README.md) | `master` @ `2c980bb` (2026-08-01) | CPM source | `engine.image` mip chain (`stb_image_resize2`) |
| [Dear ImGui](DearImGui/README.md) | `v1.92.9b-docking` | CPM source build | `engine.app` (`imgui.base` static + UI layer), game UI |
| [imgui-module bindings](imgui-module/README.md) | `v1.92.9b` | CMake download into shared dir | `imgui` C++20 module target |
| [toml++](tomlplusplus/README.md) | 3.4.0 (port `3.4.0#1`) | vcpkg; CPM fallback pins `v3.4.0` | `engine.core` config file codec |
| [GLFW](GLFW/README.md) | 3.4 (port `3.4#1`) | vcpkg (no CPM fallback) | `engine.app` platform/input/window/player backend |
| [Box2D](box2d/README.md) | 3.1.1 (manifest override) | vcpkg | `engine.physics` |
| [{fmt}](fmt/README.md) | 12.1.0 | vcpkg | Mango (no direct PPR include) |
| [zlib](zlib/README.md) | 1.3.1 | vcpkg | Mango |
| [libdeflate](libdeflate/README.md) | 1.25 | vcpkg | Mango |
| [zstd](zstd/README.md) | 1.5.7 | vcpkg | Mango |
| [lcms2](lcms/README.md) | 2.17 | vcpkg | Mango |
| [simdjson](simdjson/README.md) | 4.2.3 | vcpkg | Mango `import3d` (glTF JSON) |
| [Vulkan-Headers](Vulkan-Headers/README.md) | 1.4.328.0 | vcpkg | see entry — Slang-RHI fetches its own headers today |
| [WinPixEventRuntime](WinPixEventRuntime/README.md) | 1.0.240308001 | vcpkg | `engine.core` HAL PIX markers (`pix3.h`) |
| [FastNoiseLite](FastNoiseLite/README.md) | 1.1.1 @ `7ccfbc16…` | CPM pinned source + local patch | `engine.sim` worldgen noise |
| [FreeType](FreeType/README.md) | 2.13.3 (port 0) | vcpkg registry port | opt-in M4 ImGui FreeType rasterizer evaluation |

Transitive second-order fetches (Slang-RHI's DXC, Agility SDK, NVAPI, Dawn,
its own Vulkan-Headers copy, its own WinPixEventRuntime copy) are documented
inside the Slang-RHI entry rather than as separate folders, because they are
fetch targets of that library's pinned CMake, not declarations of this
repository.

Adding a dependency: extend `vcpkg.json` and/or `cmake/external/<Library>.cmake`,
link it from the owning target only, and add a folder here with the model
above (upstream, pin + why, preserved license notice + hash, integration
scope). Streaming a third-party type across a module boundary is not allowed
without an owning wrapper module per AGENTS.md.
