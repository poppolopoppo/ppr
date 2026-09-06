# include/

## Responsibility

Public header root for the PPR engine. Holds the single non-module header `pP/Macros.h` — the engine-wide
preprocessor vocabulary (build-mode detection, compiler attributes, assertions, logging, error handling, RAII
helpers) included by every module partition and the game entry point.

## Design

- Only one shipped file: `pP/Macros.h`. It is NOT a module — preprocessor-only, `#include`d in the global
  fragment (`module; #include "pP/Macros.h"`) before any module scanning.
- Test-only macros (`PPR_UNIT_TEST`, `PPR_TEST_ASSERT`) live separately in
  `lib/engine/tests/include/pP/UnitTest.h` and are NOT shipped here.

## Flow

Every `.cppm`/`.cpp` starts with `module; #include "pP/Macros.h"`; macros expand at preprocessor time and are
available uniformly across all partitions and `game/main.cpp`.

## Integration

- Consumed by: every engine module partition, `game/main.cpp`, all test targets.
- See [pP/Macros.h codemap](pP/codemap.md) for the full macro catalog.

## Key Files

- `pP/Macros.h` — engine-wide preprocessor macros (see [pP/codemap.md](pP/codemap.md)).
