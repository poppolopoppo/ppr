# cmake/external/

## Responsibility
External dependency CMake configuration — CPM source-based packages, vcpkg manifest mode, and generated module bindings for third-party libraries used by the PPR engine.

## Design
- **CPM packages** (`cmake/Dependencies.cmake`): Fetches dependencies from GitHub using CPMAddPackage. Current packages:
  - `GLFW` — window/input library, `find_package(glfw3 REQUIRED CONFIG)`, CXX_MODULE_STD OFF
  - `Mango` — image processing library, requires `CMAKE_PREFIX_PATH` for vcpkg-installed deps, AVX/AVX2/SSE2 enabled, OpenGL/Vulkan/Examples disabled
  - `rapidhash` — very fast hash functions, interface imported target
  - `SlangRHI` — shader language RHI abstraction, SLANG_RHI_FETCH_SLANG ON, unity build ON, CXX_MODULE_STD OFF (workaround for root-scope std module link leak)
  - `STB` — header-only public domain libraries, interface imported target
- **DearImGui** (`cmake/external/DearImGui.cmake`): Uses CPM to download `imgui` v1.92.9b-docking from GitHub. Generates C++20 module bindings (`imgui.cppm`, `imgui_internal.cppm`) from the `stripe2933/imgui-module` repo at v1.92.9b. The generated modules are cached at `CMAKE_BINARY_DIR/../imgui_module_bindings` shared across presets. `ImGuiModule` target has `CXX_MODULE_STD OFF` to avoid the CMake 4.4 root-scope std module synthetic target link leak (LNK2001).
- **vcpkg integration** (`cmake/VCPkg.cmake`): If `VCPKG_ROOT` environment variable is set, uses vcpkg toolchain file. Dependencies (fmt, zlib, libdeflate, zstd, lcms, simdjson, glfw3, vulkan-headers) are resolved via vcpkg manifest mode from `vcpkg.json`. Without vcpkg, CPM fetches from source.
- **CMake 4.4 workaround**: DearImGui explicitly sets `CXX_MODULE_STD OFF` on `ImGuiModule` because it is defined at root scope (via `include()`d cmake file), where CMake's synthetic `std` module target (`@cmake_cxx_std.lib`) is referenced on link lines as a bare `@`-prefixed name — the leading `@` is MSVC response-file syntax, so the linker drops it and every link fails with LNK2001 unresolved externals for std module implicit inline definitions.

## Flow
1. User configures with or without `VCPKG_ROOT`
2. If vcpkg: `vcpkg` preset sets toolchain file, triplet, and adds `VCPKG_INSTALLED_DIR` to `CMAKE_PREFIX_PATH`
3. If no vcpkg: `CMakeLists.txt` includes `cmake/Dependencies.cmake` which uses CPMAddPackage to fetch from GitHub
4. Generated module bindings for DearImGui are downloaded once and cached
5. Each package has `CXX_MODULE_STD OFF` set to prevent the root-scope `@cmake_cxx_std.lib` synthetic-target link leak (LNK2001)
6. Engine targets use `setup_ppr_project()` which applies `CXX_MODULE_STD ON` selectively

## Integration
- Root `CMakeLists.txt` includes `cmake/Dependencies.cmake` after `include(VCPkg.cmake)`
- `setup_ppr_project(target INTERNAL_PUBLIC_DEPS EngineCore EngineApp EngineMath EngineShader EngineRHI)` links engine modules
- Engine RHI target links `slang-rhi` which transitively provides Slang session/registry
- DearImGui `ImGuiModule` provides C++20 module bindings consumed via `import imgui;` in engine code
- vcpkg-managed deps are linked via `target_link_libraries`; CPM-managed deps provide imported targets

## Key Files
- `cmake/Dependencies.cmake` — CPM packages: GLFW, Mango, rapidhash, SlangRHI, STB, DearImGui
- `cmake/external/SlangRHI.cmake` — Slang-RHI CPM, unity build, CXX_MODULE_STD OFF, D3D11/Optix/CUDA disabled
- `cmake/external/DearImGui.cmake` — CPM imgui v1.92.9b-docking, generated module bindings download and caching
- `cmake/external/GLFW.cmake` — find_package(glfw3), SYSTEM includes, CXX_MODULE_STD OFF
- `cmake/external/Mango.cmake` — CPM mango with vcpkg prefix path, AVX/AVX2/SSE2, no examples/opengl/vulkan
- `cmake/external/rapidhash.cmake` — CPM rapidhash, interface imported target
- `cmake/external/STB.cmake` — CPM stb, interface imported target
- `vcpkg.json` — vcpkg manifest: fmt, zlib, libdeflate, zstd, lcms, glfw, and more
- `CMakePresets.json` — `vcpkg` preset injects toolchain; `windows-default` sets `VCPKG_TARGET_TRIPLET` x64-windows