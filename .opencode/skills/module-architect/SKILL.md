---
name: module-architect
description: >
  Guides the creation, modification, and maintenance of C++20 module-based
  code in the PPR game engine. Use this skill whenever you need to add a new
  module partition, create a new top-level library, register module sources
  in CMakeLists.txt, write test modules, or understand the module declaration
  and implementation patterns used throughout `lib/engine/`.
---

# Module Architect Guide

Step-by-step reference for the PPR engine's C++20 module system (engine.core
partitions, top-level libraries, test modules, platform HAL).

---

## Step 1 — Understand the file naming conventions

Every source file in the module system follows a strict naming convention
that encodes its role (interface vs implementation), its library, and
its partition hierarchy.

| Role | Extension | Example | Convention |
|------|-----------|---------|------------|
| Module interface (primary or partition) | `.cppm` | `Core.Assert.cppm` | `Library.Partition.cppm` |
| Module implementation (definitions) | `.cpp` | `Core.Assert.cpp` | `Library.Partition.cpp` |
| Legacy macro-only header | `.h` | `pP/Macros.h` | Only `Macros.h` uses `.h` |
| Platform HAL source | `.cpp` | `Core.HAL.windows.Memory.cpp` | `Core.HAL.<platform>.<Area>.cpp` |
| Umbrella module | `.cppm` | `Core.cppm` | `Library.cppm` (no dot) |

Key rules:
- **Library modules** are named `engine.core`, `engine.math`, `engine.rhi`, `engine.app`.
  - The file is `Core.cppm`, `Math.cppm`, `RHI.cppm`, `App.cppm` respectively.
- **Partitions** use a colon in the module name: `engine.core:assert`, `engine.core:memory.arena`.
  - The file is `Core.Assert.cppm` (dots replace colons in the filename).
  - Dots in partition names map to hierarchy: `engine.core:memory.arena` is filed as `Core.Memory.Arena.cppm`.
- **Implementation files** mirror the interface filename but use `.cpp`:
  - `Core.Memory.Arena.cppm` interface => `Core.Memory.Arena.cpp` implementation.
- **Test modules** use underscores in partition names: `engine.tests:core_arena`.
  - The file is `Core.Memory.Arena.Tests.cppm`.
- **Top-level tests umbrella**: `engine.tests` in `Core.Tests.cppm`.
- **Macro header**: only `include/pP/Macros.h` uses `.h` extension. All other headers
  are eliminated in favour of modules.

---

## Step 2 — Write a module interface file (.cppm)

Every `.cppm` file follows a strict structure with three regions:

```
module;                                    // Global module fragment
#include "pP/Macros.h"                     // Only Macros.h allowed here

export module engine.core:your_partition;  // Module declaration

import :assert;                            // Sibling partition imports
import :hal;
import std;                                // Standard library import

export namespace pP { ... }               // Exported declarations
```

Format requirements:
1. **Line 1**: `module;` (global module fragment marker, standalone on its own line).
2. **Line 2**: `#include "pP/Macros.h"` — the only `#include` permitted in module files.
   Exception: top-level libraries that wrap third-party code may `#include <external/header.h>`
   in the global module fragment (see `Math.cppm` and `RHI.cppm`).
3. **Line 3**: Blank line (optional but conventional).
4. **The `export module` line**: no blank line between `module;` and `export module`.
   Example: `export module engine.core:memory.arena;`
5. **Import section**: Import sibling partitions with `import :partition_name;`.
   Import the standard library with `import std;`.
6. **Export section**: Wrap all exported declarations in `export namespace pP { ... }`.
   For sub-namespaces: `export namespace pP::hal { ... }` or `export namespace pP::mem { ... }`.
   For extending `std` namespace: `export namespace std { ... }` (see `Core.Strings.cppm`).

Rules for imports:
- `import std;` is always present (the engine uses `import std` experimental).
- Sibling partitions use `import :partition_name;` — no leading library prefix.
- Cross-library imports use the full module name: `import engine.core;`.
- Never `import` a partition from outside its parent module.
- Never use `#include` for standard library headers in module files.

---

## Step 3 — Write an implementation file (.cpp)

Implementation files provide the definitions for declarations in the corresponding
`.cppm` interface. They also use a strict structure:

```
module;                                    // Global module fragment
#include "pP/Macros.h"                     // Only Macros.h allowed

module engine.core;                        // Re-open the primary module
import :your_partition;                    // Import the partition to implement
import std;                                // Standard library import

namespace pP { ... }                       // Definitions (no export keyword)
```

Key differences from the interface file:
- `module engine.core;` instead of `export module engine.core:partition;`
  — this re-opens the primary module namespace for definitions.
- `import :your_partition;` — imports the partition whose declarations you implement.
- No `export` keyword on any declarations in the implementation file.
- Template explicit instantiations go at file scope:
  ```cpp
  template class pP::mem::Arena<pP::mem::HugePage>;
  template class pP::mem::Arena<pP::mem::SmallPage>;
  ```

---

## Step 4 — Register the partition in the umbrella module

Every partition must be re-exported from the top-level umbrella module file
so that consumers who `import engine.core;` see all types.

File: `lib/engine/core/Core.cppm`

```
module;
export module engine.core;

export import :assert;
export import :memory;
export import :memory.arena;
// ... all 30 partitions
```

Add a new line: `export import :new_partition_name;`

The partition name after the colon uses dots for hierarchy:
- `:memory.arena` not `:memory_arena`
- `:containers.hash_map` not `:containers_hash_map`

The umbrella must list partitions in alphabetical order (by partition name).

---

## Step 5 — Register in CMakeLists.txt

Each module interface (`.cppm`) and implementation (`.cpp`) file must be
listed in the library's `CMakeLists.txt`.

File: `lib/engine/core/CMakeLists.txt`

```
target_sources(EngineCore
    PUBLIC
        FILE_SET CXX_MODULES FILES
            Core.Assert.cppm
            Core.Memory.Arena.cppm
            Core.Memory.PagePool.cppm
            ...
            Core.cppm                      # Umbrella — comes last
    PRIVATE
        Core.Assert.cpp
        Core.Memory.Arena.cpp
        Core.Memory.PagePool.cpp
        ...
)
```

Rules:
- **PUBLIC** `FILE_SET CXX_MODULES FILES` lists all `.cppm` files (interfaces).
  The umbrella `Core.cppm` must appear in this list.
- **PRIVATE** lists all `.cpp` implementation files.
- Partition `.cppm` files and their `.cpp` counterparts are listed separately
  — their association is implicit via the module declaration, not CMake.
- Platform HAL sources listed via a variable (`${HAL_PLATFORM_SOURCES}`) in the
  PRIVATE section.

---

## Step 6 — Create a new top-level library

A top-level library (like `engine.math`, `engine.rhi`, `engine.app`) has its own
`CMakeLists.txt` and uses the `setup_ppr_project` function.

Minimal `CMakeLists.txt`:

```cmake
add_library(EngineNewLib)

target_sources(EngineNewLib
    PUBLIC
        FILE_SET CXX_MODULES FILES
            NewLib.cppm
    PRIVATE
        NewLib.impl.cpp
)

setup_ppr_project(EngineNewLib
    INTERNAL_PUBLIC_DEPS EngineCore          # for import engine.core;
    EXTERNAL_SYSTEM_PRIVATE_DEPS some_ext_lib
)
```

Arguments to `setup_ppr_project`:
- `INTERNAL_PUBLIC_DEPS` — other engine targets this library depends on
  (linked PUBLIC so consumers transitively get them).
- `EXTERNAL_SYSTEM_PRIVATE_DEPS` — third-party libraries (linked PRIVATE).

The library's `.cppm` file declares the module:
```
module;
export module engine.new_lib;
import engine.core;
import std;

export namespace pP { ... }
```

---

## Step 7 — Create test modules

Test modules follow a parallel structure to engine partitions.

**File naming**: `Core.<Area>.Tests.cppm` (e.g., `Core.Memory.Arena.Tests.cppm`).

**Module naming**: `engine.tests:core_arena` (underscores, not dots, after the colon).

**Namespace**: `export namespace pP::tests { ... }`.

**Structure**:

```
module;
#include "pP/Macros.h"

export module engine.tests:core_arena;

import engine.core;
import std;

export namespace pP::tests {

    namespace Arena {
        PPR_UNIT_TEST(lifo_operations) {
            // ...
        };
    }

    PPR_UNIT_TEST(arena) {
        _.recurse({
            Arena::lifo_operations,
            // ...
        });
    };
}
```

**Register in umbrella**: `lib/engine/tests/core/Core.Tests.cppm`:
```
export module engine.tests;
import :core_arena;
```

**Register in CMake**: `lib/engine/tests/CMakeLists.txt`:
```
target_sources(EngineTests
    PUBLIC
        FILE_SET CXX_MODULES FILES
            core/Core.Memory.Arena.Tests.cppm
            core/Core.Tests.cppm
)
```

The test executable already links `EngineCore` via `setup_ppr_project`.

---

## Step 8 — Cross-module imports guide

| Import statement | When to use | Example file |
|---|---|---|
| `import std;` | Every module file needs the standard library | All `.cppm` and `.cpp` files |
| `import :partition;` | From within the same library, importing a sibling partition | `Core.Assert.cppm` imports `:function_ref` |
| `import engine.core;` | From a different library or executable, importing the entire core module | `App.cppm`, `Application.cpp`, test files |
| `import engine.core;` then `import :partition;` | **Never** mix full module and partition imports in the same file — use one or the other | — |
| `import engine.math;` | From a library that depends on engine.math | Any file in a target with `INTERNAL_PUBLIC_DEPS EngineMath` |

Rules:
- Partition imports (`:name`) are only valid from within the same parent module.
  Files in `engine.app` cannot `import :assert` — they must `import engine.core;`.
- Implementation files re-open the primary module (`module engine.core;`) and then
  import the specific partition they implement (`import :memory.arena;`).
- Test files `import engine.core;` (the full module), not individual partitions.

---

## Step 9 — Platform HAL implementation pattern

HAL implementations live in `lib/engine/core/<platform>/` where `<platform>` is
`windows`, `linux`, `darwin`, or `generic`.

**File naming**: `Core.HAL.<platform>.<Area>.cpp`
Examples:
- `windows/Core.HAL.windows.Memory.cpp`
- `windows/Core.HAL.windows.Debugger.cpp`
- `windows/Core.HAL.windows.Io.cpp`
- `windows/Core.HAL.windows.Timer.cpp`

**CMake registration**: In `lib/engine/core/CMakeLists.txt`, a variable collects
platform sources:
```cmake
set(HAL_PLATFORM_SOURCES
    ${PPR_HAL_PLATFORM}/Core.HAL.${PPR_HAL_PLATFORM}.Debugger.cpp
    ${PPR_HAL_PLATFORM}/Core.HAL.${PPR_HAL_PLATFORM}.Memory.cpp
    ...
)
if(PPR_HAL_PLATFORM STREQUAL "windows")
    list(APPEND HAL_PLATFORM_SOURCES
        windows/Core.HAL.windows.Random.cpp        # Windows-only
        windows/Core.HAL.windows.RingBuffer.cpp    # Windows-only
    )
endif()
```

The variable `${HAL_PLATFORM_SOURCES}` is then placed in the PRIVATE section
of `target_sources`.

**Structure of each HAL file**:
```
module;

#include "Core.HAL.windows.include.h"   // Platform-specific includes
#include <Memoryapi.h>

module engine.core;

import :assert;
import :hal;
import :memory;
import :memory.poison;
import std;

namespace pP::hal {
    // Implementations of declarations from Core.HAL.cppm
    const std::size_t page_size = ...;
    void pageFree(void *ptr, std::size_t size) { ... }
}
```

The HAL partition is registered as `export import :hal;` in `Core.cppm` and its
interface is `Core.HAL.cppm` (in the PUBLIC CXX_MODULES set).

---

## Step 10 — Common pitfalls

### Pitfall 1: Missing `module;` global module fragment
Every `.cppm` and module `.cpp` file **must** begin with `module;` on its own line,
before any `#include` directives. Forgetting this causes the compiler to treat the
file as a regular translation unit and reject `export module`.

### Pitfall 2: Wrong `import :partition;` vs full module
- Inside `engine.core` files: use `import :assert;` (colon + partition name).
- Outside `engine.core` (e.g., `engine.app`): use `import engine.core;`.
- Implementation files: `module engine.core;` then `import :memory.arena;`.
- **Never use both `import engine.core;` and `import :assert;` in the same file.**

### Pitfall 3: Forgetting umbrella registration
A partition that is not listed in `Core.cppm` via `export import :partition;`
will not be visible to consumers who `import engine.core;`. They would have
to know about and directly import the partition, which is unsupported.
Always add the `export import :name;` line when creating a new partition.

### Pitfall 4: Linker errors from missing CMake registration
Every new `.cppm` and `.cpp` file **must** be added to the appropriate
`target_sources(...)` call in the library's `CMakeLists.txt`. Missing this
causes "module not found" or unresolved symbol errors. The file is compiled only
when listed in `target_sources`.

### Pitfall 5: Test module naming uses underscores
Partition names in tests (`engine.tests:core_arena`) use underscores, while
engine core partitions (`engine.core:memory.arena`) use dots. This is intentional
and must be followed.

### Pitfall 6: Using `#include` instead of `import`
- No `#include <vector>` — use `import std;`.
- No `#include "Core.Types.h"` — there are no `.h` files besides `Macros.h`.
- The only `#include` allowed in module files is `#include "pP/Macros.h"`.

### Pitfall 7: Implementation file module declaration
Implementation files use `module engine.core;` (not `export module engine.core;`
and not `module engine.core:partition;`). The form `module engine.core;` re-opens
the primary module for definition attachment.

### Pitfall 8: Re-exporting non-exported entities
In `.cppm` files, everything in `export namespace pP { ... }` is exported.
In `.cpp` files, nothing is exported (the `export` keyword is forbidden).
Trying to define an exported entity in a `.cpp` file that wasn't declared in
the `.cppm` will cause a linker error.

---

## Step 11 — Commit rules

When adding a new module partition or library, every commit must contain:

1. **The new source file(s)**: `.cppm` (interface) and `.cpp` (implementation), or
   just `.cppm` if the partition has no non-trivial definitions.
2. **CMakeLists.txt update**: Register the new file(s) in `target_sources`.
3. **Umbrella update**: Add `export import :partition;` to the umbrella `Core.cppm`
   (or `Core.Tests.cppm` for test partitions).

All three changes go in the **same commit**. Do not split source creation
and build registration across commits.

---

## Constraints

- Only `Macros.h` is included via `#include` in module files (exception: third-party
  wrappers like `Math.cppm` may include external headers in the global module fragment).
- All module files must begin with the global module fragment (`module;`).
- Partition names after the colon use dots in engine core (`:memory.arena`) and
  underscores in tests (`:core_arena`).
- Filenames follow the pattern `Core.<PartitionName>.cppm` where dots in the partition
  name correspond to dots in the filename.
- Every partition must be listed in the umbrella module and in `CMakeLists.txt`.
- CMake registration uses `FILE_SET CXX_MODULES` for `.cppm` files and `PRIVATE`
  sources for `.cpp` files.
- Test modules use `import engine.core;` (the full module), never partition imports.
- Platform HAL sources follow the `Core.HAL.<platform>.<Area>.cpp` naming and are
  collected via `${HAL_PLATFORM_SOURCES}` in CMake.
