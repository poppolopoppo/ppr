---
name: build-system
description: >
  CMake build system configuration, preset management, dependency resolution,
  MSVC module workarounds, and sanitizer configuration for the PPR game engine.
  Use this skill when your task involves cmake, presets, build targets,
  dependencies, or linker errors.
---

# Build System Guide

## Coverage

### CMake Version & Module Support
- **CMake 4.3+** required (see `CMakeLists.txt` line 1: `cmake_minimum_required(VERSION 4.3 FATAL_ERROR)`)
- C++23 modules enabled via `CMAKE_CXX_MODULE_STD ON` and `CMAKE_EXPERIMENTAL_CXX_IMPORT_STD`
- Experimental build database export enabled for CMake < 4.4
- Single-config Ninja generators used (see CMakePresets.json `default` preset)

### CMake Presets
Available presets (from `CMakePresets.json`):

| Preset | Platform | Compiler | Build Type | Developer Mode |
|--------|----------|----------|------------|----------------|
| `msvc-dev` | Windows | MSVC (`cl`) | Debug | ON |
| `msvc-rel` | Windows | MSVC (`cl`) | Release | OFF |
| `clang-cl-dev` | Windows | Clang-cl | Debug | ON |
| `clang-cl-rel` | Windows | Clang-cl | Release | OFF |
| `clang-dev` | Linux/Darwin | Clang | Debug | ON |
| `clang-rel` | Linux/Darwin | Clang | Release | OFF |
| `gcc-dev` | Linux/Darwin | GCC | Debug | ON (hidden — no module support) |
| `gcc-rel` | Linux/Darwin | GCC | Release | OFF (hidden — no module support) |
| `developer` | Any | Inherits | Debug | ON |
| `vcpkg` | Any | Inherits | Debug | Inherits |

**Recommended build workflow:**
```bash
# Configure
cmake --preset msvc-dev

# Build
cmake --build out/build/msvc-dev

# Run tests
ctest --preset msvc-dev
# Or directly:
out/build/msvc-dev/EngineCoreTests
out/build/msvc-dev/EngineAppTests
```

### `setup_ppr_project()` Function
Defined in `cmake/Compilers.cmake`. Every PPR target must use this function:

```cmake
setup_ppr_project(MyTarget
    INTERNAL_PUBLIC_DEPS EngineCore EngineMath
    EXTERNAL_SYSTEM_PRIVATE_DEPS imgui slang-rhi
    EXTERNAL_SYSTEM_PUBLIC_DEPS glfw
)
```

Parameters:
- `INTERNAL_PUBLIC_DEPS` — Internal engine library targets (linked PUBLIC)
- `EXTERNAL_SYSTEM_PRIVATE_DEPS` — External library targets (linked PRIVATE, SYSTEM includes)
- `EXTERNAL_SYSTEM_PUBLIC_DEPS` — External library targets (linked PUBLIC, SYSTEM includes)

The function:
1. Enables `CXX_MODULE_STD` and `cxx_std_23`
2. Sets compiler warnings (`PPR_PROJECT_WARNINGS_CXX`)
3. Adds `include/` directory for `pP/Macros.h`
4. Calls `enable_sanitizers()`
5. Links all specified dependencies
6. Disables compiler cache for module targets via `ppr_disable_compiler_cache()`

### Dependency Management
Dependencies are managed via two systems:

**vcpkg (manifest mode):** `fmt`, `zlib`, `libdeflate`, `zstd`, `lcms`, `glfw3`, `vulkan-headers`, `slang`
- Installed automatically via `vcpkg.json` manifest
- Requires `VCPKG_ROOT` environment variable set

**CPM (source-based):** `slang-rhi`, `mango`, `rapidhash`, `stb`, `imgui`, `glfw` (fallback)
- Each in its own file under `cmake/external/`
- Cached to `out/cpm_cache` (configurable via `CPM_SOURCE_CACHE`)
- `glfw` has both vcpkg and CPM sources — vcpkg preferred, CPM as fallback

### MSVC Module Workarounds
Key MSVC-specific configurations in `cmake/compiler/MSVC.cmake`:

1. **vcpkg toolchain auto-detection** — If `VCPKG_ROOT` is set and no toolchain file is configured, it auto-detects
2. **`/Z7` debug info** — Uses `CMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded` instead of `/Zi` for ccache compatibility and parallel build safety
3. **`/EHsc`** — C++ exceptions enabled (required by C++ Standard Library module)
4. **`/utf-8`** — Applied to every target that builds or imports modules
5. **`/Zc:__cplusplus`** — Ensures `__cplusplus` reports correct standard value
6. **`/permissive-`** — Standards conformance mode
7. **ASAN STL annotations** — When `PPR_ENABLE_SANITIZER_ADDRESS` is ON, disables STL ASan annotations (`_DISABLE_STRING_ANNOTATION`, etc.) to prevent LNK2038 mismatch between std module and user modules
8. **clang-tidy disabled** — Commented out in `CMakeLists.txt` line 71 due to module incompatibility

### Sanitizer Configuration
Defined in `cmake/Sanitizers.cmake`, called automatically by `setup_ppr_project()`:

**MSVC support:**
- `PPR_ENABLE_SANITIZER_ADDRESS` — Adds `/fsanitize=address`
- Leak, UB, Thread, Memory sanitizers — Not supported (status messages emitted)

**GCC/Clang support:**
- `PPR_ENABLE_SANITIZER_ADDRESS` — `-fsanitize=address`
- `PPR_ENABLE_SANITIZER_LEAK` — `-fsanitize=leak`
- `PPR_ENABLE_SANITIZER_UNDEFINED` — `-fsanitize=undefined`
- `PPR_ENABLE_SANITIZER_THREAD` — `-fsanitize=thread` (conflicts with ASan/LSan)
- `PPR_ENABLE_SANITIZER_MEMORY` — `-fsanitize=memory` (Clang only, conflicts with ASan/TSan/LSan)
- `PPR_ENABLE_COVERAGE` — `--coverage -O0 -g`

**Developer mode** (`PPR_ENABLE_DEVELOPER_MODE=ON`) auto-enables:
- `ENABLE_CACHE` (ccache for non-module TUs)
- `PPR_ENABLE_SANITIZER_ADDRESS`
- `PPR_ENABLE_SANITIZER_UNDEFINED`
- `PPR_ENABLE_CPPCHECK`
- `PPR_WARNINGS_AS_ERRORS`

### Compiler Cache
Defined in `cmake/Cache.cmake`:

- Enabled when `ENABLE_CACHE=ON` (auto-set in developer mode)
- Supports `ccache` and `sccache` (configurable via `CACHE_OPTION` cache variable)
- Sets `CMAKE_C_COMPILER_LAUNCHER` and `CMAKE_CXX_COMPILER_LAUNCHER` globally
- **Module targets must opt out** via `ppr_disable_compiler_cache()` — ccache doesn't track BMI content
- Uses `/Z7` (not `/Zi`) to avoid PDB lock contention in parallel builds

### Static Analyzers
Defined in `cmake/StaticAnalyzers.cmake`:

- **cppcheck** — Enabled via `PPR_ENABLE_CPPCHECK`, runs with `--enable=style,performance,warning,portability`
- **clang-tidy** — Enabled per-target via `PPR_ENABLE_CLANG_TIDY`, currently disabled in developer mode due to module incompatibility

### HAL Platform Selection
Defined in `cmake/HAL.cmake`:
- `WIN32` → `windows`
- `APPLE` → `darwin`
- `UNIX AND NOT APPLE` → `linux`
- Fallback → `generic`

Platform sources are under `lib/engine/core/hal/<platform>/` with filenames like `Core.HAL.<platform>.<Area>.cpp`.

### Test Targets
- `EngineCoreTests` — GLFW-free core tests (`lib/engine/tests/core/`)
- `EngineAppTests` — GLFW-linked app tests (`lib/engine/tests/app/`)
- `EngineTestsShared` — Shared test infrastructure (`lib/engine/tests/shared/`)
- CTest integration via `enable_testing()` in `CMakeLists.txt` line 51

### Common Build Issues
- **Linker errors with modules:** Ensure `CXX_MODULE_STD ON` is set on all targets
- **MSVC ASAN mismatch:** STL annotations are disabled when ASAN is on — don't re-enable them
- **ccache + modules:** Module targets must call `ppr_disable_compiler_cache()` — ccache doesn't handle BMI content
- **vcpkg not found:** Set `VCPKG_ROOT` environment variable or use `cmake --preset vcpkg`
- **GCC modules:** GCC presets are hidden (`gcc-dev`, `gcc-rel`) — GCC does not support C++20 modules yet