# include/

## Responsibility
Public header root for the PPR engine. Holds the single non-module header `pP/Macros.h` — the engine-wide preprocessor vocabulary (assertions, logging, compiler attributes, error handling, RAII helpers) included by every module partition and the game entry point.

## Design
- Only one file lives here: `pP/Macros.h`. It is NOT a module — it is preprocessor-only and must be `#include`d before any module scanning.
- Test-only macros (`PPR_UNIT_TEST`, `PPR_TEST_ASSERT`) live separately in `lib/engine/tests/include/pP/UnitTest.h` and are NOT shipped here.

## Flow
Every `.cppm` and `.cpp` starts with `module; #include "pP/Macros.h"` (or `#include "pP/Macros.h"` before `import` statements). Macros expand at preprocessor time and are available uniformly across all partitions.

## Integration
- Consumed by: every engine module partition, `game/main.cpp`, and all test targets.
- See [pP/Macros.h codemap](pP/codemap.md) for the full macro catalog.

## Key Files
- `pP/Macros.h` — engine-wide preprocessor macros (see [pP/codemap.md](pP/codemap.md)).
