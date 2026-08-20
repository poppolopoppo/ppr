# cmake/compiler/

## Responsibility
Compiler-specific CMake configuration for MSVC, clang-cl, Clang, and GCC toolchains. Applies platform-specific flags, warning suppression, sanitizer support, and C++20 module workarounds for the PPR engine.

## Design
- **MSVC** (`cmake/compiler/MSVC.cmake`): The default and primary toolchain. Applies `/bigobj` for large module interface TUs (critical for consistent `@cmake_cxx_std.lib` synthetic targets). Enables `/utf-8`, `/EHsc`, `/Zc:__cplusplus`. Provides `PPR_PROJECT_WARNINGS_CXX` with baseline `/W4` and targeted suppressions (`/wd4201`, `/w14242`, etc.). Handles `PPR_ENABLE_SANITIZER_ADDRESS` via `/D_ANNOTATE_STL`. Release perf flags (`/arch:AVX2`, `/Gw`) are gated behind `PPR_RELEASE_PERF_FLAGS`. Edit & Continue (`/ZI`) is supported only in the `msvc-live` preset with Ninja.
- **Clang** (`cmake/compiler/Clang.cmake`): Uses `-stdlib=libc++` for libc++ module support. Applies comprehensive warning flags (`-Wall`, `-Wextra`, `-Wshadow`, etc.). When `PPR_WARNINGS_AS_ERRORS` is set, adds `-Werror`. GCC configuration (`cmake/compiler/GCC.cmake`) extends Clang warnings with `-Wmisleading-indentation`, `-Wduplicated-cond`, etc.
- **clang-cl** (`cmake/compiler/Clang.cmake` is included via Compilers.cmake when `CMAKE_CXX_COMPILER_ID MATCHES ".*Clang"`): Same flags as Clang but targeted for MSVC compatibility mode; used via the `clang-cl-dev`/`clang-cl-rel` presets which set `CMAKE_C_COMPILER:clang-cl` and `CMAKE_CXX_COMPILER:clang-cl`.
- **GCC** (`cmake/compiler/GCC.cmake`): Includes Clang.cmake base, adds GCC-specific warnings. Marked `**DO NOT WORK WITH MODULES**` in presets due to lack of module support.

## Flow
1. `Compilers.cmake` detects `CMAKE_CXX_COMPILER_ID` and includes the matching compiler config
2. Global `add_compile_options` genex expressions apply flags per-compiler+config
3. Warning suppression (`/wd...` / `-w...`) targets known false positives with module code
4. Sanitizer support is enabled via `cmake/Sanitizers.cmake` which dispatches per compiler
5. `setup_ppr_project()` in `Compilers.cmake` sets `CXX_MODULE_STD ON`, `cxx_std_23` compile features

## Integration
- Included from root `CMakeLists.txt` via `include(Compilers)`
- Flags propagate to all sub-targets via `target_compile_options` / `add_compile_options`
- `PPR_PROJECT_WARNINGS_CXX` is defined per-compiler (`MSVC.cmake`/`Clang.cmake`/`GCC.cmake`) and applied to targets by `Compilers.cmake::setup_ppr_project()`
- `CXX_MODULE_STD ON` is set globally in `Compilers.cmake::setup_ppr_project()`
- MSVC `/bigobj` is applied globally to prevent divergent `@cmake_cxx_std.lib` synth targets
- DearImGui `ImGuiModule` has `CXX_MODULE_STD OFF` to avoid root-scope `@cmake_cxx_std.lib` LNK2001 issues

## Key Files
- `cmake/compiler/MSVC.cmake` — /bigobj, /utf-8, /EHsc, /Zc:__cplusplus, PPR_PROJECT_WARNINGS_CXX, /arch:AVX2 /Gw genex
- `cmake/compiler/Clang.cmake` — -stdlib=libc++, -Wall -Wextra long list, -Werror when enabled
- `cmake/compiler/GCC.cmake` — Extends Clang warnings with GCC-specific options
- `cmake/Compilers.cmake` — Dispatcher: MSVC/Clang/GCC inclusion, calls setup_ppr_project
- `CMakeLists.txt` — `include(Compilers)`, `CMAKE_CXX_STANDARD 23`, `CMAKE_CXX_MODULE_STD ON`
- `CMakePresets.json` — msvc-dev (cl + PPR_ENABLE_DEVELOPER_MODE ON), msvc-rel, clang-cl-dev/clang-cl-rel, msvc-live (Ninja + /ZI + no sanitizers)