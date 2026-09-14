# cmake/external/

## Responsibility

External dependency CMake configuration — CPM source packages, vcpkg manifest mode, and the generated C++20
module bindings for third-party libraries consumed by the engine.

## Design

- **CPM packages** (`cmake/Dependencies.cmake` + per-lib files): GLFW (`find_package(glfw3)`, SYSTEM includes),
  Mango (vcpkg prefix path, AVX/AVX2/SSE2, no examples/OpenGL/Vulkan), rapidhash (interface target), SlangRHI
  (`SLANG_RHI_FETCH_SLANG ON`, unity build, D3D11/Optix/CUDA off), and STB (interface target).
- **Mango normalization** (`Mango.cmake`): its concrete `mango*` targets set `CXX_MODULE_STD OFF` locally and
  remove incompatible interface compile options so their differing flags cannot create incompatible std-module
  synth targets. Other external dependencies use only the workarounds their own target structure requires.
- **DearImGui** (`DearImGui.cmake`, `imgui` v1.92.9b-docking via CPM): split into `imgui.base` (static lib over
  `imgui*.cpp`, SYSTEM includes, `CXX_MODULE_STD OFF`) and `imgui` (module lib over downloaded
  `imgui.cppm`/`imgui_internal.cppm` "Combined Module" bindings from `stripe2933/imgui-module` v1.92.9b —
  `using`-re-export of `imgui.h`, full `ImGuiContext` for `ErrorCallback`, `IMGUI_HAS_DOCK`-guarded docking
  symbols). Bindings download once (TLS_VERIFY, fatal on failure) into the preset-shared
  `${CMAKE_BINARY_DIR}/../imgui_module_bindings` so a single physical `export module imgui;` exists — multiple
  per-preset copies make CLion report "Module 'imgui' is ambiguous".
- **CMake 4.4 workaround**: both `imgui.base` and `imgui` pin `CXX_MODULE_STD OFF` (root-scope `include()`d
  targets reference the synthetic `@cmake_cxx_std.lib` as a bare `@`-name on link lines; MSVC parses the
  leading `@` as response-file syntax and every link fails with LNK2001 on std-module inline definitions).
  Re-test on newer CMake — see AGENTS.md "CMake Version Tracking".
- **vcpkg** (`cmake/VCPkg.cmake` + `vcpkg.json`): when `VCPKG_ROOT` is set (or the `vcpkg` preset toolchain),
  manifest-mode deps (fmt, zlib, libdeflate, zstd, lcms, simdjson, glfw3, vulkan-headers, …) resolve from
  `VCPKG_INSTALLED_DIR` on `CMAKE_PREFIX_PATH`; otherwise CPM fetches from GitHub.

## Flow

1. Configure with/without `VCPKG_ROOT` → toolchain + triplet selection.
2. CPM fetches sources (or vcpkg supplies installed trees); ImGui module bindings download once into the shared
  dir.
3. `imgui.base` builds static; `imgui` builds the two module units and links `imgui.base` PUBLIC.
4. Engine targets link via `setup_ppr_project` (`engine.app` links `imgui.base` private + `imgui` PUBLIC).

## Integration

- Root `CMakeLists.txt` includes `VCPkg` then `Dependencies`; `engine.rhi` consumes `slang-rhi`/`slang`,
  `engine.math`/`engine.app` consume `mango`, `engine.app` consumes `glfw` + `imgui`, `engine.core` consumes
  `rapidhash`.
- Engine code writes `import imgui;` (never `#include <imgui.h>`) — resolved through the PUBLIC `imgui` link.

## Key Files

- `cmake/Dependencies.cmake` — package list (GLFW, Mango, rapidhash, SlangRHI, STB, DearImGui).
- `DearImGui.cmake` — `imgui.base` static + `imgui` module, shared bindings cache, `CXX_MODULE_STD OFF`.
- `SlangRHI.cmake` — Slang-RHI CPM, unity build, `CXX_MODULE_STD OFF`.
- `GLFW.cmake` / `Mango.cmake` / `rapidhash.cmake` / `STB.cmake` — per-dependency fetch/link settings.
- `vcpkg.json` — manifest (fmt, zlib, libdeflate, zstd, lcms, glfw, …).
