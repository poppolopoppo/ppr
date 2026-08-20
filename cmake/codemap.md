# cmake/

## Responsibility
Top-level CMake configuration — root CMakeLists.txt, CMakePresets.json, and vcpkg.json integration. Sets up the build environment, compiler toolchains, sanitizers, and external dependency discovery for the entire PPR engine.

## Design
- **Root CMakeLists.txt** (`CMakeLists.txt`): Sets C++23 standard, enables module support (`CMAKE_CXX_SCAN_FOR_MODULES ON`, `CMAKE_CXX_MODULE_STD ON`), defines project `PPR`, and includes all `cmake/` module files. Validates incompatible `PPR_ENABLE_*` combinations and enforces preset constraints (e.g., `PPR_EDIT_AND_CONTINUE` requires Ninja + MSVC + Debug).
- **CMakePresets.json**: Named single-config presets (`msvc-dev`, `msvc-rel`, `clang-cl-dev`, `clang-cl-rel`, `msvc-live`, `gcc-dev`/`gcc-rel`, `clang-dev`/`clang-rel`, `developer`, `vcpkg`, `default`). Uses Ninja single-config to avoid CMake 4.4 genex leak with C++ module synthetic targets.
- **vcpkg.json**: VCPKG root/manifest configuration; when `VCPKG_ROOT` is set, the `vcpkg` preset injects the vcpkg toolchain file. Without vcpkg, CPM fetches dependencies from source.
- **Compiler configuration**: `cmake/Compilers.cmake` dispatches to `MSVC`, `Clang`, or `GCC` sub-configurations. MSVC gets `/bigobj`, `/utf-8`, `/EHsc`, warning suppression, and release perf flags (`/arch:AVX2`, `/Gw`). Clang gets `-stdlib=libc++`. Sanitizers are handled via `cmake/Sanitizers.cmake`.
- **External dependencies**: `cmake/Dependencies.cmake` includes CPM packages (GLFW, Mango, rapidhash, SlangRHI, STB) and vcpkg manifest mode. DearImGui uses generated C++20 module bindings.

## Flow
1. User runs `cmake --preset <name>` or `cmake -S . -B build`
2. `CMakeLists.txt` prevents in-source builds, sets C++23/std23, enables module scanning
3. Compiler module (`Compilers.cmake`) applies toolchain-specific flags and warning settings
4. Sanitizers module enables ASAN/TSan/MSAN based on `PPR_ENABLE_SANITIZER_*` options
5. Dependencies are fetched: vcpkg (if `VCPKG_ROOT` set) or CPM from GitHub
6. Engine libs (`EngineCore`, `EngineMath`, `EngineRHI`, `EngineShader`, `EngineApp`) built as C++20 modules
7. `game/CMakeLists.txt` builds `VideoGameApp` with `setup_ppr_project`, copies shader assets POST_BUILD

## Integration
- Root `CMakeLists.txt` includes: `PreventInSourceBuilds`, `VCPkg`, `HAL`, `Compilers`, `Sanitizers`, `StaticAnalyzers`, `Cache`, `Dependencies`
- `setup_ppr_project(target ... INTERNAL_PUBLIC_DEPS ...)` helper in `Compilers.cmake` links engine targets and applies CXX_MODULE_STD
- Engine modules re-exported via umbrella `import` in each library's umbrella `.cppm`
- Viewport-scoped service stores (`m_scene_services`, `m_ui_services`) chained via `ServicesStore` parent-chain fallback

## Key Files
- `CMakeLists.txt` — root configuration, options, option guards, module setup
- `CMakePresets.json` — 14 named presets: default, developer, vcpkg, windows-default, unix-like-default, msvc-dev, msvc-rel, clang-cl-dev, clang-cl-rel, msvc-live, gcc-dev, gcc-rel, clang-dev, clang-rel
- `vcpkg.json` — vcpkg manifest mode configuration
- `cmake/Compilers.cmake` — MSVC/Clang/GCC flag settings, warning suppression, sanitizer integration
- `cmake/Sanitizers.cmake` — ASAN/TSan/MSAN enable per compiler (MSVC gets /fsanitize=address, GCC/Clang gets -fsanitize)
- `cmake/Dependencies.cmake` — CPM packages: GLFW, Mango, rapidhash, SlangRHI, STB, DearImGui
- `cmake/compiler/MSVC.cmake` — /bigobj, /utf-8, /EHsc, warning suppression, /Zc:__cplusplus, PPR_PROJECT_WARNINGS_CXX
- `cmake/compiler/Clang.cmake` — -stdlib=libc++, warning flags, -Werror when PPR_WARNINGS_AS_ERRORS
- `cmake/compiler/GCC.cmake` — Extends Clang warnings with GCC-specific options
- `cmake/external/SlangRHI.cmake` — Slang-RHI CPM package, module std off, unity build on
- `cmake/external/DearImGui.cmake` — CPM imgui v1.92.9b-docking, generated module bindings download
- `cmake/external/GLFW.cmake` — find_package(glfw3), SYSTEM includes, CXX_MODULE_STD OFF