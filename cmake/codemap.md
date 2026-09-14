# cmake/

## Responsibility

Top-level CMake configuration — root `CMakeLists.txt`, `CMakePresets.json`, and dependency/toolchain wiring.
Sets up C++23 modules, compiler toolchains, sanitizers, and external dependency discovery for the engine,
`app.game`, and tests.

## Design

- **Root `CMakeLists.txt`**: C++23 (`CMAKE_CXX_STANDARD 23`, `SCAN_FOR_MODULES ON`,
  `EXPORT_COMPILE_COMMANDS ON`), project `PPR`; version-gated `CMAKE_EXPERIMENTAL_CXX_IMPORT_STD` UUIDs
  (4.2/4.3/4.4+) + `EXPORT_BUILD_DATABASE` before `project()`; `CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES`
  exposes `include/` (Macros.h) to module scanning; prevents in-source builds, `enable_testing()`, developer-mode
  flag fan-out (ASan + UBSan + cppcheck + warnings-as-errors); guards reject bad `PPR_ENABLE_*` combos
  (`PPR_EDIT_AND_CONTINUE` requires Ninja + MSVC + Debug and no ASan; THREAD excludes ADDRESS/LEAK; MEMORY
  excludes ADDRESS/THREAD/LEAK).
- **`CMakePresets.json`** (single-config Ninja throughout — avoids the CMake 4.4 multi-config genex leak into
  C++ module BMIs): `default` (Debug + CPM/vcpkg cache vars), `developer` (+ `PPR_ENABLE_DEVELOPER_MODE`),
  `vcpkg` (toolchain from `$VCPKG_ROOT`), hidden `windows-default` (MSVC/Clang: `VS_SEGMENT_HEAP_ALLOWLIST`
  for game+tests, `SegmentHeap.cmake`, `x64-windows` triplet) and `unix-like-default` (inherits `vcpkg`),
  `msvc-dev`/`msvc-rel`, `clang-cl-dev`/`clang-cl-rel`, `msvc-live` (Debug + `/ZI` Edit & Continue, no
  sanitizers/ccache, `PPR_RELEASE_PERF_FLAGS OFF`, `PPR_EDIT_AND_CONTINUE ON`), `clang-dev`/`clang-rel`,
  hidden `gcc-dev`/`gcc-rel` (**no modules**).
- **`setup_ppr_project(target INTERNAL_PUBLIC_DEPS … EXTERNAL_SYSTEM_PRIVATE_DEPS … …)`**
  (`cmake/Compilers.cmake`): the single helper every PPR target uses — applies target-local
  `cxx_std_23`/`CXX_MODULE_STD` opt-in,
  warning sets, and link edges. `app.game` links all five engine modules; `engine.app` additionally links
  `imgui` PUBLIC so `import imgui;` resolves from importers.
- **Runtime/shader delivery** (`game/CMakeLists.txt`): `POST_BUILD` copies `$<TARGET_RUNTIME_DLLS:app.game>`
  next to the exe (no hardcoded DLL list) and copies `assets/shaders` → `<exe>/shaders`.

## Flow

1. `cmake --preset <name>` (single-config Ninja) → toolchain file (vcpkg when rooted) + `CMAKE_BUILD_TYPE`.
2. `Compilers.cmake` dispatches to `compiler/MSVC|Clang|GCC`; `Sanitizers.cmake` applies ASAN/UBSAN per option.
3. `Dependencies.cmake` + `cmake/external/*` fetch CPM packages / resolve vcpkg manifests.
4. Engine libs build as C++20 modules; `app.game` links, then POST_BUILD stages DLLs + shaders.

## Integration

- Root `CMakeLists.txt` includes: `PreventInSourceBuilds`, `VCPkg`, `HAL`, `Compilers`, `Sanitizers`,
  `StaticAnalyzers`, `Cache`, `Dependencies`.
- Compiler specifics: see [compiler/codemap.md](compiler/codemap.md); third-party wiring:
  see [external/codemap.md](external/codemap.md).
- PPR targets opt in to `CXX_MODULE_STD` through `setup_ppr_project()`; external exceptions set it OFF locally.

## Key Files

- `CMakeLists.txt` — root configuration, options, option guards, module setup.
- `CMakePresets.json` — curated preset set above (`default`, `developer`, `vcpkg`, `msvc-dev/rel`,
  `clang-cl-dev/rel`, `msvc-live`, `clang-dev/rel`, hidden `windows/unix-like-default`, hidden `gcc-dev/rel`).
- `vcpkg.json` — vcpkg manifest mode configuration.
- `cmake/Compilers.cmake` — dispatcher + `setup_ppr_project`.
- `cmake/Sanitizers.cmake` — ASAN/UBSAN enable per compiler.
- `cmake/Dependencies.cmake` — CPM package list.
