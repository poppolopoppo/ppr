# PPR Game Engine

A modern C++23 game engine built with C++20 Modules, leveraging [Slang-RHI](https://github.com/shader-slang/slang-rhi) for cross-platform rendering and [Mango](https://github.com/t0rak/mango) for math.

## Features

- **C++23 Modules** - Clean module-based architecture with `.cppm` interface files
- **Cross-Platform Rendering** - Hardware abstraction via Slang-RHI supporting Vulkan, DirectX 12, and more
- **Advanced Math Library** - Vector, matrix, and quaternion math
- **Custom Memory Management** - Tiered allocators (Arena/ScopedArena/ScratchPad, Pooling/LocalCache, Fallback/Threshold/InSitu) with PMR/STL wrappers
- **Type-Safe Containers** - `StableVector`, `SparseVector`, `HashMap`, `HashSet`, `Stack`, `RingBuffer`
- **Platform Abstraction Layer** - Unified HAL for filesystem, memory, async I/O, and OS interactions
- **Shader Compilation** - Slang shader compilation
- **Dear ImGui Integration** - UI service with listener-based input dispatch
- **Built-in Testing** - Lightweight unit test framework with `PPR_UNIT_TEST` and CTest integration
- **Assertions System** - Tiered assertions (`PPR_ASSERT`, `PPR_VERIFY`, `PPR_ENSURE`)
- **error_code Lifecycle** - Consistent error propagation across all services and APIs

## Project Structure

For a navigable repository map, see [codemap.md](codemap.md). Contributors
should follow the engineering contract in [AGENTS.md](AGENTS.md).

```
ppr/
├── assets/            # Game assets (shaders, etc.)
├── lib/engine/
│   ├── core/          # Core utilities
│   │   ├── memory/    #   Allocators (GPA, Arena, PagePool, ...)
│   │   ├── containers/#   Containers (HashMap, StableVector, ...)
│   │   ├── concurrency/#  Concurrency (channels, events, contexts)
│   │   ├── io/        #   Async I/O, file watchers
│   │   ├── hal/       #   Platform abstraction (windows, linux, darwin, generic)
│   │   └── function/  #   Function wrappers (Callback, function_ref)
│   ├── math/          # Math module (wraps mango::math)
│   ├── shader/        # Shader compilation
│   ├── rhi/           # Rendering hardware interface (wraps slang-rhi)
│   ├── app/           # Application layer (slim Application + ApplicationEditor)
│   │   ├── input/     #   Input actions, keys, listeners, devices
│   │   ├── platform/  #   IPlatform interface
│   │   │   └── glfw/  #     GLFW backend
│   │   ├── player/    #   Player identity store + state graph
│   │   ├── renderer/  #   Content-free Renderer + TrianglePass (Types header-only)
│   │   ├── scene/     #   Camera + controller
│   │   ├── service/   #   Service contracts (client/input/player/ui/window)
│   │   ├── ui/        #   ImGui overlay service
│   │   └── window/    #   Window service + viewport geometry
│   └── tests/         # Unit tests (core, app, shared)
├── game/              # Game application entry point
├── cmake/             # CMake modules and toolchain files
└── include/           # pP/Macros.h only
```

## Prerequisites

- **CMake** 4.3 or later
- **C++23 compiler**: MSVC 17.8+ or Clang 18+ for supported PPR module-build
  presets. GCC 14+ may meet the language prerequisite, but the hidden `gcc-*`
  presets are non-module and are not supported validation paths.
- **Vulkan SDK** (for Vulkan backend)
- **Git** with submodules support

## Building

```bash
# Clone the repository
git clone https://github.com/poppolopoppo/ppr.git
cd ppr

# Configure with CMake presets (recommended)
cmake --preset msvc-dev

# Build
cmake --build out/build/msvc-dev
```

Common presets are `msvc-dev`, `msvc-live`, `msvc-rel`, `clang-cl-dev`,
`clang-cl-rel`, `clang-dev`, and `clang-rel`. The hidden `gcc-*` presets do not
support C++ modules. `msvc-live` is the Edit & Continue preset (`/ZI` + `/DEBUG:FULL` +
`/INCREMENTAL` + `/OPT:NOREF,NOICF` + `/LTCG:OFF` + `/PDBTMCACHE`, live-only `/MDd`;
misconfigurations fail at configure time). `msvc-rel` is the shipping preset (pinned
`/O2` + `/Ob2`, `/GL` + `/LTCG`, `/OPT:REF,ICF`, `/INCREMENTAL:NO`, `/DEBUG` with a stripped
`app.game.stripped.pdb` beside the full `app.game.pdb`; Release `/Zi`, dev `/Z7`) and sets
`BUILD_TESTING=OFF`.

### Developer Mode

Enable additional checks and sanitizers:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DPPR_ENABLE_DEVELOPER_MODE=ON
cmake --build build
```

### Available CMake Options

| Option | Description | Default |
|--------|-------------|---------|
| `PPR_ENABLE_DEVELOPER_MODE` | Enable warnings-as-errors and sanitizers | OFF |
| `PPR_ENABLE_COVERAGE` | Coverage reporting (gcc/clang) | OFF |
| `PPR_ENABLE_SANITIZER_ADDRESS` | Address sanitizer | OFF |
| `PPR_ENABLE_SANITIZER_UNDEFINED` | Undefined behavior sanitizer | OFF |
| `PPR_ENABLE_CLANG_TIDY` | Run clang-tidy | OFF |
| `PPR_ENABLE_CPPCHECK` | Run cppcheck | OFF |
| `PPR_ENABLE_UNITY_BUILD` | Unity build for faster compilation | OFF |
| `PPR_WARNINGS_AS_ERRORS` | Treat warnings as errors | OFF |
| `PPR_RELEASE_PERF_FLAGS` | Release `/O2` + `/Ob2` + `/GL` + `/Gw` + `/Zc:checkGwOdr` | ON |
| `PPR_ENABLE_AVX2` | Opt-in AVX2 codegen (`/arch:AVX2`, 2013+ CPU); default min-spec unchanged | OFF |
| `PPR_EDIT_AND_CONTINUE` | MSVC Edit & Continue (Debug, `/ZI` + live link set) | OFF |
| `ENABLE_CACHE` | Enable compiler cache (ccache) for non-module TUs | OFF (ON in dev mode) |
| `PPR_HAL_PLATFORM` | HAL platform override (windows, linux, darwin, generic) | auto-detected |

## Dependencies

Managed via [vcpkg](https://github.com/microsoft/vcpkg) and [CPM.cmake](https://github.com/cpm-cmake/CPM.cmake) (see `vcpkg.json` for the manifest and `cmake/external/` for resolution):

### Vcpkg Packages
- `fmt` - Formatting library
- `zlib`, `libdeflate`, `zstd` - Compression
- `lcms` - Color management
- `simdjson` - Fast JSON parsing
- `glfw3` - Windowing and input
- `vulkan-headers` - Vulkan API headers

### CPM Packages
- `slang-rhi` - Rendering hardware interface
- `mango` - Math library
- `rapidhash` - Fast hashing
- `stb` - Image loading (stb_image)
- `imgui` - Dear ImGui UI library

## Usage

Snippets below are illustrative; the module codemaps own the API contracts.

### Using Math Module

```cpp
import engine.math;

pP::float3 position{1.0f, 2.0f, 3.0f};
pP::float4x4 view = pP::lookAt(position, target, up);
auto projected = pP::perspective(60.0f, aspect, 0.1f, 1000.0f);
```

### Container Usage

```cpp
import engine.core;

pP::StableVector<int> vec = {1, 2, 3, 4, 5};
pP::HashMap<int, std::string> map{{1, "one"}, {2, "two"}};
pP::SparseVector<float> sparse;
auto handle = sparse.add(42.0f);
```

## Module Structure

| Module | Description |
|--------|-------------|
| `engine.core` | Core foundation (types, memory, containers, concurrency, IO, services, opaque, HAL, function) |
| `engine.math` | Math types and functions (float2-4, float3x3, float4x4, Quaternion, easing) |
| `engine.shader` | Shader compilation and `IShaderService` |
| `engine.rhi` | Rendering interface (device, buffers, shaders, command buffers) |
| `engine.app` | Application framework (slim lifecycle + editor client, services, renderer, UI) |

## Testing

Two separate test executables are provided:

- **`engine.tests.core`** — GLFW-free; tests memory, containers, concurrency, IO, strings, services
- **`engine.tests.app`** — Links GLFW; tests platform-dependent features

They share a common test infrastructure library (`engine.tests`) in `lib/engine/tests/shared/`.
Test targets and the `Core.UnitTest` partition (`PPR_ENABLE_UNIT_TEST`, conditional
`export import :unit_test`) are present unless `BUILD_TESTING` is explicitly `OFF`
(`msvc-rel` sets it `OFF`). The editor partition is not gated — `game/main.cpp` imports
`ApplicationEditor` unconditionally.

### Via CTest

```bash
ctest --test-dir out/build/msvc-dev --output-on-failure
```

### Direct Execution

```bash
out/build/msvc-dev/engine.tests.core --shuffle
out/build/msvc-dev/engine.tests.app --run-test app/player
```

### Options

Full flag list lives with the shared test infrastructure (`lib/engine/tests/shared/`, `parseCli()`).

### Defining Tests

Each suite keeps one exported root (`core` / `app`, decl-only `.cppm` +
out-of-line def is the MSVC C1001 workaround) and a set of thematic
private group files (`lib/engine/tests/core/Core.Allocator.Tests.cpp`, …):
a `module engine.tests.<suite>;` impl unit holding non-exported `detail::`
leaves plus one TU-local `const UnitTest` and one non-exported
`const UnitTest &<node>Tests() noexcept` accessor per root-visible node
(file-local sub-groups need no accessor; `memory`/`containers` assembled in
`Core.Tests.cpp`). Direct-root singleton leaves use a TU-local copy plus
accessor (`const UnitTest <leaf> = detail::<leaf>;` +
`const UnitTest &<leaf>Tests() noexcept { return <leaf>; }`, called as
`<leaf>Tests()` — never a new `Named`; see `module-architect`). Add a
new leaf to the existing thematic file; add a new group file (CMake PRIVATE
SOURCES in the same change) plus one accessor forward-declare and one recurse
call in the suite root only for a new thematic area:

```cpp
PPR_UNIT_TEST(my_test) {
    PPR_TEST_ASSERT(condition);
};
```

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## Contributing

Contributions are welcome! Please ensure:
- Code follows [AGENTS.md](AGENTS.md)
- New features include unit tests
- CMake builds cleanly with `PPR_ENABLE_DEVELOPER_MODE=ON`

