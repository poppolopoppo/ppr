# PPR Game Engine

A modern C++23 game engine built with C++20 Modules, leveraging [Slang-RHI](https://github.com/shader-slang/slang-rhi) for cross-platform rendering and [Mango](https://github.com/t0rak/mango) for math and image processing.

## Features

- **C++23 Modules** - Clean module-based architecture with `.cppm` interface files
- **Cross-Platform Rendering** - Hardware abstraction via Slang-RHI supporting Vulkan, DirectX 12, and more
- **Advanced Math Library** - Full vector/matrix/quaternion math with easing functions and spline interpolation
- **Custom Memory Management** - GPA (General Purpose Allocator), Arena, PagePool, BitmapTree, and Slab allocators
- **Type-Safe Containers** - `StableVector`, `SparseVector`, `HashMap`, `HashSet`, `Stack`, `RingBuffer`
- **Platform Abstraction Layer** - Unified HAL for filesystem, memory, async I/O, and OS interactions
- **Shader Compilation** - Slang shader compilation with hot-reload and background compilation
- **Dear ImGui Integration** - UI service with listener-based input dispatch
- **Built-in Testing** - Lightweight unit test framework with `PPR_UNIT_TEST`, fork/crash support, CTest integration
- **Assertions System** - Tiered assertions (`PPR_ASSERT`, `PPR_VERIFY`, `PPR_ENSURE`)
- **error_code Lifecycle** - Consistent error propagation across all services and APIs

## Project Structure

> **Note**: For the canonical and most up-to-date architecture overview, module descriptions, and partition counts, see [AGENTS.md](AGENTS.md) → "Architecture Overview." The structure below is a high-level summary.

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
│   ├── shader/        # Shader compilation and hot-reload
│   ├── rhi/           # Rendering hardware interface (wraps slang-rhi)
│   ├── app/           # Application layer with GLFW + ImGui
│   └── tests/         # Unit tests (core, app, shared)
├── game/              # Game application entry point
├── cmake/             # CMake modules and toolchain files
└── include/           # pP/Macros.h only
```

## Prerequisites

- **CMake** 4.3 or later
- **C++23 compliant compiler** (MSVC 17.8+, GCC 14+, Clang 18+)
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

Available presets: `msvc-dev` (default Windows), `msvc-rel`, `clang-cl-dev`, `clang-cl-rel`, `clang-dev`, `clang-rel`.

### Developer Mode

Enable additional checks and sanitizers:

```bash
cmake --preset developer
```

Or manually:

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
| `ENABLE_CACHE` | Enable compiler cache (ccache) for non-module TUs | OFF (ON in dev mode) |
| `PPR_HAL_PLATFORM` | HAL platform override (windows, linux, darwin, generic) | auto-detected |

## Dependencies

Managed via [vcpkg](https://github.com/microsoft/vcpkg) and [CPM.cmake](https://github.com/cpm-cmake/CPM.cmake):

### Vcpkg Packages
- `fmt` - Formatting library
- `zlib`, `libdeflate`, `zstd` - Compression
- `lcms` - Color management
- `simdjson` - Fast JSON parsing
- `glfw3` - Windowing and input
- `vulkan-headers` - Vulkan API headers
- `slang` - Shader compiler (fetched automatically via slang-rhi)

### CPM Packages
- `slang-rhi` - Rendering hardware interface
- `mango` - Math and image library
- `rapidhash` - Fast hashing
- `stb` - Image loading (stb_image)
- `imgui` - Dear ImGui UI library (v1.91.8-docking)

## Usage

### Basic Application

```cpp
import engine.core;
import engine.math;
import engine.shader;
import engine.rhi;
import engine.app;
import std;

int main(int argc, char* argv[]) {
    pP::Application app("MyGame", std::span{argv, argc});
    std::error_code err = app.run();
    return err.value();
}
```

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
| `engine.core` | Core exports (assert, arena, containers, enums, hal, hash_map, memory, strings) |
| `engine.math` | Math types and functions (float2-4, float3x3, float4x4, Quaternion, easing) |
| `engine.shader` | Shader compilation, hot-reload, IShaderService |
| `engine.rhi` | Rendering interface (device, buffers, shaders, command buffers) |
| `engine.app` | Application framework (window, input, lifecycle, UI) |

## Coding Standards

- **No raw loops** - Prefer algorithms and ranges
- **`constexpr` everywhere** - Compile-time evaluation when possible
- **`[[nodiscard]]`** - Mark functions returning important values
- **`PPR_FORCE_INLINE`** - Hot-path optimization
- **`noexcept`** - Mark non-throwing functions
- **Comments** - Only for genuinely surprising or non-obvious code that cannot be clarified through naming or structure alone
- **Macros** - Only from `include/pP/Macros.h` (assertions, logging, inlining, unit tests)

## Testing

Two separate test executables are provided:

- **`EngineCoreTests`** — GLFW-free; tests memory, containers, concurrency, IO, strings, services
- **`EngineAppTests`** — Links GLFW; tests platform-dependent features

They share a common test infrastructure library (`EngineTestsShared`) in `lib/engine/tests/shared/`.

### Via CTest

```bash
ctest --preset msvc-dev
```

### Direct Execution

```bash
out/build/msvc-dev/EngineCoreTests --shuffle
out/build/msvc-dev/EngineAppTests --run-test App.Player
```

### Options

```
--run-test <path>    Run specific test (e.g., Core.Memory)
--shuffle [<seed>]   Randomize test order
--loop <N>           Repeat N times
--help               Show all options
```

### Defining Tests

```cpp
PPR_UNIT_TEST(my_test) {
    PPR_ASSERT(condition);
};
```

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## Contributing

Contributions are welcome! Please ensure:
- Code follows the project's coding standards
- New features include unit tests
- CMake builds cleanly with `PPR_ENABLE_DEVELOPER_MODE=ON`
