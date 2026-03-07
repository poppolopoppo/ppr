# PPR Game Engine

A modern C++23 game engine built with C++20 Modules, leveraging [Slang-RHI](https://github.com/shader-slang/slang-rhi) for cross-platform rendering and [Mango](https://github.com/t0rak/mango) for math and image processing.

## Features

- **C++23 Modules** - Clean module-based architecture with `.cppm` interface files
- **Cross-Platform Rendering** - Hardware abstraction via Slang-RHI supporting Vulkan, DirectX 12, and more
- **Advanced Math Library** - Full vector/matrix/quaternion math with easing functions and spline interpolation
- **Custom Memory Management** - GPA (General Purpose Allocator), Arena, PagePool, and BitmapTree allocators
- **Type-Safe Containers** - `StableVector`, `SparseVector`, `HashMap`, `HashSet`, `Stack`, `RingBuffer`
- **Platform Abstraction Layer** - Unified HAL for filesystem, memory, and OS interactions
- **Built-in Testing** - Lightweight unit test framework with `PPR_UNIT_TEST` macro
- **Assertions System** - Tiered assertions (`PPR_ASSERT`, `PPR_VERIFY`, `PPR_ENSURE`)

## Project Structure

```
ppr/
├── lib/engine/
│   ├── core/          # Core utilities (containers, memory, strings, HAL)
│   ├── math/          # Math module (wraps mango::math)
│   ├── rhi/           # Rendering hardware interface (wraps slang-rhi)
│   ├── app/           # Application layer with GLFW integration
│   └── tests/         # Unit tests
├── game/              # Game application entry point
├── cmake/             # CMake modules and toolchain files
└── include/           # Legacy headers and macros
```

## Prerequisites

- **CMake** 4.2 or later
- **C++23 compliant compiler** (MSVC 17.8+, GCC 14+, Clang 18+)
- **Vulkan SDK** (for Vulkan backend)
- **Git** with submodules support

## Building

```bash
# Clone the repository
git clone https://github.com/poppolopoppo/ppr.git
cd ppr

# Configure with CMake
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build --config Release
```

### Developer Mode

Enable additional checks and sanitizers:

```bash
cmake -B build -DPPR_ENABLE_DEVELOPER_MODE=ON
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

## Dependencies

Managed via [vcpkg](https://github.com/microsoft/vcpkg) and [CPM.cmake](https://github.com/cpm-cmake/CPM.cmake):

### Vcpkg Packages
- `fmt` - Formatting library
- `zlib`, `libdeflate`, `zstd` - Compression
- `lcms` - Color management
- `simdjson` - Fast JSON parsing
- `glfw3` - Windowing and input
- `vulkan-headers` - Vulkan API headers

### CPM Packages
- `slang-rhi` - Rendering hardware interface
- `mango` - Math and image library
- `rapidhash` - Fast hashing
- `stb` - Image loading (stb_image)

## Usage

### Basic Application

```cpp
import engine.core;
import engine.math;
import engine.rhi;
import engine.app;

int main(int argc, char* argv[]) {
    app::Application app("MyGame", std::span{argv, argc});
    return app.run();
}
```

### Using Math Module

```cpp
import engine.math;

math::float3 position{1.0f, 2.0f, 3.0f};
math::float4x4 view = math::lookAt(position, target, up);
auto projected = math::perspective(60.0f, aspect, 0.1f, 1000.0f);
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
| `engine.rhi` | Rendering interface (device, buffers, shaders, command buffers) |
| `engine.app` | Application framework (window, input, lifecycle) |

## Coding Standards

- **No raw loops** - Prefer algorithms and ranges
- **`constexpr` everywhere** - Compile-time evaluation when possible
- **`[[nodiscard]]`** - Mark functions returning important values
- **`PPR_FORCE_INLINE`** - Hot-path optimization
- **`noexcept`** - Mark non-throwing functions
- **No comments** - Self-documenting code
- **No macros** - Except those in `ppr/Macros.h`

## Testing

Run the built-in unit tests:

```cpp
import engine.core;
pP::UnitTest::run(pP::tests::core);
```

Define tests with:

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
